/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 The LineageOS Project
 */
#include "ApThermalPolicy.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <numeric>

namespace aidl::android::hardware::thermal {
namespace {

// CPUFreqMax tables: A520FXXU4CREF ssrm_a5y17_xx.xml and
// A720SKSU5CUJ2 ssrm_a7y17_xx.xml. Temperatures are AP/PST deci-Celsius.
constexpr std::array<int32_t, 7> kCpuLimits = {
        -1, 1794000, 1690000, 1586000, 1352000, 1144000, 1014000};
constexpr int kTripStep = 20;
// Native policy hysteresis: avoid repeatedly changing limits near a boundary.
constexpr int kReleaseGap = 20;
// Fixed-window AP filtering used by the Pie profile, sampled by the HAL at 2 s.
constexpr size_t kWindowSize = 15;
constexpr int64_t kResetAfterMs = 10000;
constexpr size_t kSeverityCount = static_cast<size_t>(ThrottlingSeverity::SHUTDOWN) + 1;

int64_t bootTimeMs() {
    timespec time{};
    if (clock_gettime(CLOCK_BOOTTIME, &time) != 0) return -1;
    return static_cast<int64_t>(time.tv_sec) * 1000 + time.tv_nsec / 1000000;
}

bool readApTemperature(const std::string& path, int* value) {
    std::string text;
    return ::android::base::ReadFileToString(path, &text) &&
            ::android::base::ParseInt(::android::base::Trim(text), value) &&
            *value >= -200 && *value <= 900;
}

ThrottlingSeverity severityForLevel(size_t level) {
    if (level == 0) return ThrottlingSeverity::NONE;
    // Group successive stock CPU caps into Android's coarser severity levels.
    if (level <= 2) return ThrottlingSeverity::LIGHT;
    if (level <= 4) return ThrottlingSeverity::MODERATE;
    return ThrottlingSeverity::SEVERE;
}

}  // namespace

bool ApThermalPolicy::initialize() {
    const auto model = ::android::base::GetProperty("ro.vendor.universal7880.device", "");
    if (model == "a5y17lte") {
        firstTrip_ = 420;
        skinOffset_ = 77;
    } else if (model == "a7y17lte") {
        firstTrip_ = 430;
        skinOffset_ = 80;
    } else {
        LOG(ERROR) << "No AP thermal profile for model: " << model;
        return true;  // Die-temperature reporting and protection remain available.
    }

    // Use the AP thermistor, never a die-temperature fallback for the skin estimate.
    for (const auto* candidate : {"/sys/devices/virtual/sec/sec-ap-thermistor/temperature",
                                  "/sys/class/sec/sec-ap-thermistor/temperature"}) {
        int value;
        if (readApTemperature(candidate, &value)) {
            path_ = candidate;
            break;
        }
    }
    if (path_.empty()) {
        LOG(ERROR) << "Cannot read the AP thermistor for " << model;
        return false;
    }
    for (size_t i = 0; i < cpuRequests_.size(); ++i) {
        const auto path = "/dev/cluster" + std::to_string(i) + "_freq_max";
        cpuRequests_[i].reset(TEMP_FAILURE_RETRY(open(path.c_str(), O_WRONLY | O_CLOEXEC)));
        if (cpuRequests_[i].get() < 0) {
            PLOG(ERROR) << "Cannot open " << path;
            return false;
        }
    }

    threshold_.type = TemperatureType::SKIN;
    threshold_.name = "ap-skin-estimate";
    threshold_.hotThrottlingThresholds.assign(kSeverityCount, NAN);
    threshold_.coldThrottlingThresholds.assign(kSeverityCount, NAN);
    for (const auto severity : {ThrottlingSeverity::LIGHT, ThrottlingSeverity::MODERATE,
                                ThrottlingSeverity::SEVERE}) {
        const auto index = static_cast<size_t>(severity);
        threshold_.hotThrottlingThresholds[index] =
                (firstTrip_ + 2 * (static_cast<int>(index) - 1) * kTripStep - skinOffset_) / 10.0f;
    }
    temperature_.type = TemperatureType::SKIN;
    temperature_.name = threshold_.name;
    coolingDevice_.type = CoolingType::CPU;
    coolingDevice_.name = "ap-cpu-limit";
    enabled_ = true;
    LOG(INFO) << "Using " << model << " AP thermal profile at " << path_;
    return update();
}

bool ApThermalPolicy::setCpuLimit(size_t level) {
    if (appliedLevel_ == static_cast<int>(level)) return true;
    const int32_t limit = kCpuLimits[level];
    for (const auto& request : cpuRequests_) {
        // qos.c accepts a native s32. Text of any other size is parsed as hex.
        // Each open file owns a separate request, removed automatically on close.
        const ssize_t written = TEMP_FAILURE_RETRY(write(request.get(), &limit, sizeof(limit)));
        if (written != static_cast<ssize_t>(sizeof(limit))) {
            PLOG(ERROR) << "Cannot update the AP thermal CPU request";
            return false;
        }
    }
    appliedLevel_ = static_cast<int>(level);
    LOG(INFO) << "AP thermal CPU limit: " << limit << " kHz, level " << level;
    return true;
}

void ApThermalPolicy::setUnavailable() {
    std::lock_guard lock(mutex_);
    available_ = false;
}

bool ApThermalPolicy::update() {
    if (!enabled_) return true;
    int raw;
    const int64_t now = bootTimeMs();
    if (now < 0 || !readApTemperature(path_, &raw)) {
        // Preserve the last cap; a failed read must not release thermal mitigation.
        samples_.clear();
        setUnavailable();
        return false;
    }
    // CLOCK_BOOTTIME includes suspend. Do not average old, pre-suspend samples
    // into the first reading after a long sleep or delayed scheduling.
    if (lastSampleMs_ && now - lastSampleMs_ > kResetAfterMs) samples_.clear();
    lastSampleMs_ = now;
    samples_.push_back(raw);
    if (samples_.size() > kWindowSize) samples_.pop_front();
    const int filtered = std::accumulate(samples_.begin(), samples_.end(), 0) /
            static_cast<int>(samples_.size());

    size_t level = level_;
    while (level + 1 < kCpuLimits.size() &&
           filtered >= firstTrip_ + static_cast<int>(level) * kTripStep) {
        ++level;
    }
    while (level > 0 &&
           filtered < firstTrip_ + static_cast<int>(level - 1) * kTripStep - kReleaseGap) {
        --level;
    }
    if (!setCpuLimit(level)) {
        setUnavailable();
        return false;
    }
    level_ = level;
    std::lock_guard lock(mutex_);
    // Samsung estimates skin temperature as filtered AP temperature minus model gap.
    temperature_.value = (filtered - skinOffset_) / 10.0f;
    temperature_.throttlingStatus = severityForLevel(level_);
    coolingDevice_.value = level_;
    available_ = true;
    return true;
}

bool ApThermalPolicy::getTemperature(Temperature* out) const {
    std::lock_guard lock(mutex_);
    if (!available_) return false;
    *out = temperature_;
    return true;
}

CoolingDevice ApThermalPolicy::getCoolingDevice() const {
    std::lock_guard lock(mutex_);
    return coolingDevice_;
}

}  // namespace aidl::android::hardware::thermal

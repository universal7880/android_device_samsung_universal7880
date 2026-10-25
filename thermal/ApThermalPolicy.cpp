/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 The LineageOS Project
 */
#include "ApThermalPolicy.h"
#include "Policy.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <numeric>

namespace aidl::android::hardware::thermal {
namespace {

// CPUFreqMax tables: A520FXXU4CREF ssrm_a5y17_xx.xml and
// A720SKSU5CUJ2 ssrm_a7y17_xx.xml. Temperatures are AP/PST deci-Celsius.
constexpr std::array<int32_t, 7> kCpuLimits = {
        -1, 1794000, 1690000, 1586000, 1352000, 1144000, 1014000};
// AP GPU caps from the same profiles; the first cap starts 2 C after the CPU cap.
constexpr std::array<int32_t, 4> kGpuLimits = {0, 756, 548, 449};
using policy::kTripStep;
// Fixed-window AP filtering used by the Pie profile, sampled by the HAL at 2 s.
constexpr size_t kWindowSize = 15;
constexpr int64_t kResetAfterMs = 10000;
constexpr size_t kSeverityCount = static_cast<size_t>(ThrottlingSeverity::SHUTDOWN) + 1;

int64_t bootTimeMs() {
    timespec time{};
    if (clock_gettime(CLOCK_BOOTTIME, &time) != 0) return -1;
    return static_cast<int64_t>(time.tv_sec) * 1000 + time.tv_nsec / 1000000;
}

bool readTemperature(const std::string& path, int* value) {
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

ApThermalPolicy::~ApThermalPolicy() {
    if (hasChargingPolicy() && !setChargingLimit(0)) {
        LOG(ERROR) << "Cannot release the AP thermal charging request";
    }
    if (gpuRequest_.get() >= 0 && !setGpuLimit(0)) {
        LOG(ERROR) << "Cannot release the AP thermal GPU request";
    }
}

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
        if (readTemperature(candidate, &value)) {
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

    gpuRequest_.reset(TEMP_FAILURE_RETRY(open(
            "/sys/devices/11400000.mali/thermal_max_lock", O_WRONLY | O_CLOEXEC)));
    if (gpuRequest_.get() < 0) {
        if (errno != ENOENT) {
            PLOG(ERROR) << "Cannot open the AP thermal GPU request";
            return false;
        }
        // Allow the CPU policy to run when testing the HAL on an older kernel.
        LOG(WARNING) << "No independent AP thermal GPU request; keeping kernel GPU protection";
    }

    chargingRequest_.reset(TEMP_FAILURE_RETRY(open(
            "/sys/class/power_supply/battery/thermal_siop_level", O_WRONLY | O_CLOEXEC)));
    if (!hasChargingPolicy()) {
        if (errno != ENOENT) {
            PLOG(ERROR) << "Cannot open the AP thermal charging request";
            return false;
        }
        LOG(WARNING) << "No independent AP charging request; keeping kernel charging protection";
    }

    threshold_.type = TemperatureType::SKIN;
    threshold_.name = "ap-skin-estimate";
    threshold_.hotThrottlingThresholds.assign(kSeverityCount, NAN);
    threshold_.coldThrottlingThresholds.assign(kSeverityCount, NAN);
    for (const auto severity : {ThrottlingSeverity::LIGHT, ThrottlingSeverity::MODERATE,
                                ThrottlingSeverity::SEVERE}) {
        const auto index = static_cast<size_t>(severity);
        int trip = firstTrip_ + 2 * (static_cast<int>(index) - 1) * kTripStep;
        if (hasGpuPolicy()) trip = std::min(trip, firstTrip_ + static_cast<int>(index) * kTripStep);
        threshold_.hotThrottlingThresholds[index] = (trip - skinOffset_) / 10.0f;
    }
    temperature_.type = TemperatureType::SKIN;
    temperature_.name = threshold_.name;
    coolingDevice_.type = CoolingType::CPU;
    coolingDevice_.name = "ap-cpu-limit";
    gpuCoolingDevice_.type = CoolingType::GPU;
    gpuCoolingDevice_.name = "ap-gpu-limit";
    chargingCoolingDevice_.type = CoolingType::BATTERY;
    chargingCoolingDevice_.name = "ap-charging-limit";
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

bool ApThermalPolicy::setGpuLimit(size_t level) {
    if (!hasGpuPolicy() || appliedGpuLevel_ == static_cast<int>(level)) return true;
    const auto value = std::to_string(kGpuLimits[level]);
    if (lseek(gpuRequest_.get(), 0, SEEK_SET) < 0 ||
        !::android::base::WriteStringToFd(value, gpuRequest_.get())) {
        PLOG(ERROR) << "Cannot update the AP thermal GPU request";
        return false;
    }
    appliedGpuLevel_ = static_cast<int>(level);
    LOG(INFO) << "AP thermal GPU limit: " << value << " MHz, level " << level;
    return true;
}

bool ApThermalPolicy::setChargingLimit(size_t level) {
    if (!hasChargingPolicy() || appliedChargingLevel_ == static_cast<int>(level)) return true;
    const auto value = std::to_string(policy::kChargingLevels[level]);
    if (lseek(chargingRequest_.get(), 0, SEEK_SET) < 0 ||
        !::android::base::WriteStringToFd(value, chargingRequest_.get())) {
        PLOG(ERROR) << "Cannot update the AP thermal charging request";
        return false;
    }
    appliedChargingLevel_ = static_cast<int>(level);
    LOG(INFO) << "AP thermal charging limit: " << value << "%, level " << level;
    return true;
}

bool ApThermalPolicy::updateChargingLimit(int ap) {
    if (!hasChargingPolicy()) return true;
    int battery;
    if (!readTemperature("/sys/class/power_supply/battery/temp", &battery)) return false;
    // Power HAL reports framework interactivity, including screen-off and AOD.
    // A missing producer keeps the conservative screen-on charging policy.
    const bool screenOn = ::android::base::GetBoolProperty(
            "vendor.universal7880.interactive", true);
    const auto level = policy::nextChargingLevel(ap, battery, screenOn, chargingLevel_);
    if (!setChargingLimit(level)) return false;
    chargingLevel_ = level;
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
    if (now < 0 || !readTemperature(path_, &raw)) {
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

    const size_t level = policy::nextLevel(filtered, firstTrip_, level_, kCpuLimits.size());
    const size_t gpuLevel = hasGpuPolicy()
            ? policy::nextLevel(filtered, firstTrip_ + kTripStep, gpuLevel_, kGpuLimits.size()) : 0;
    if (!setCpuLimit(level) || !setGpuLimit(gpuLevel) || !updateChargingLimit(filtered)) {
        setUnavailable();
        return false;
    }
    level_ = level;
    gpuLevel_ = gpuLevel;
    std::lock_guard lock(mutex_);
    // Samsung estimates skin temperature as filtered AP temperature minus model gap.
    temperature_.value = (filtered - skinOffset_) / 10.0f;
    temperature_.throttlingStatus = std::max(severityForLevel(level_),
            static_cast<ThrottlingSeverity>(gpuLevel_));
    coolingDevice_.value = level_;
    gpuCoolingDevice_.value = gpuLevel_;
    chargingCoolingDevice_.value = chargingLevel_;
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

CoolingDevice ApThermalPolicy::getGpuCoolingDevice() const {
    std::lock_guard lock(mutex_);
    return gpuCoolingDevice_;
}

CoolingDevice ApThermalPolicy::getChargingCoolingDevice() const {
    std::lock_guard lock(mutex_);
    return chargingCoolingDevice_;
}

}  // namespace aidl::android::hardware::thermal

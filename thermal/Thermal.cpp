/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 The LineageOS Project
 */

#include "Thermal.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aidl::android::hardware::thermal {
namespace {

constexpr const char* kThermalRoot = "/sys/class/thermal/";
constexpr size_t kSeverityCount = static_cast<size_t>(ThrottlingSeverity::SHUTDOWN) + 1;

std::vector<std::string> entries(const std::string& path, const std::string& prefix) {
    std::vector<std::string> result;
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(path.c_str()), closedir);
    if (!dir) return result;
    while (auto* entry = readdir(dir.get())) {
        if (::android::base::StartsWith(entry->d_name, prefix)) {
            result.emplace_back(path + entry->d_name);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool readText(const std::string& path, std::string* value) {
    if (!::android::base::ReadFileToString(path, value)) return false;
    *value = ::android::base::Trim(*value);
    return true;
}

bool readNumber(const std::string& path, int64_t* value) {
    std::string text;
    return readText(path, &text) && ::android::base::ParseInt(text, value);
}

ndk::ScopedAStatus readError(const std::string& path) {
    return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
            EX_ILLEGAL_STATE, ("Cannot read " + path).c_str());
}

template <class T, class Type>
void filter(Type type, std::vector<T>* values) {
    values->erase(std::remove_if(values->begin(), values->end(),
                                [type](const T& value) { return value.type != type; }), values->end());
}

}  // namespace

bool Thermal::initialize() {
    // Discover by driver names: thermal_zone indices are not an ABI.
    for (const auto& path : entries(kThermalRoot, "thermal_zone")) {
        std::string name;
        if (!readText(path + "/type", &name)) continue;
        TemperatureType type;
        if (name == "therm_zone0" || name == "therm_zone1") {
            type = TemperatureType::CPU;
        } else if (name == "therm_zone2") {
            type = TemperatureType::GPU;
        } else {
            continue;
        }
        TemperatureThreshold threshold;
        threshold.name = name;
        threshold.type = type;
        threshold.hotThrottlingThresholds.assign(kSeverityCount, NAN);
        threshold.coldThrottlingThresholds.assign(kSeverityCount, NAN);
        // Only the onset of active cooling has a directly observable threshold.
        // Higher severities below are derived from cooling states, not trip indices.
        for (const auto& trip : entries(path + "/", "trip_point_")) {
            if (!::android::base::EndsWith(trip, "_type")) continue;
            std::string tripType;
            int64_t value;
            if (readText(trip, &tripType) && (tripType == "active" || tripType == "passive") &&
                readNumber(trip.substr(0, trip.size() - 5) + "_temp", &value)) {
                float& onset = threshold.hotThrottlingThresholds[
                        static_cast<size_t>(ThrottlingSeverity::LIGHT)];
                const float temperature = value / 1000.0f;
                if (std::isnan(onset) || temperature < onset) onset = temperature;
            }
        }
        std::vector<std::string> coolingPaths;
        for (const auto& link : entries(path + "/", "cdev")) {
            const auto suffix = link.substr(link.find_last_of('/') + 1);
            if (suffix.find('_') == std::string::npos) coolingPaths.push_back(link);
        }
        if (coolingPaths.empty()) {
            LOG(ERROR) << "No cooling devices bound to " << name;
            return false;
        }
        sensors_.push_back({path, coolingPaths, threshold});
    }
    for (const auto& path : entries(kThermalRoot, "cooling_device")) {
        std::string name;
        if (!readText(path + "/type", &name)) continue;
        CoolingDevice device;
        device.name = name;
        if (::android::base::StartsWith(name, "thermal-cpufreq-")) {
            device.type = CoolingType::CPU;
        } else if (::android::base::StartsWith(name, "thermal-gpufreq-")) {
            device.type = CoolingType::GPU;
        } else {
            continue;
        }
        // This driver exposes cooling states, not power budgets or energy counters.
        device.powerLimitMw = 0;
        device.powerMw = 0;
        device.timeWindowMs = 0;
        coolers_.push_back({path, device});
    }
    if (sensors_.size() != 3 || coolers_.empty()) return false;
    std::vector<Temperature> temperatures;
    std::vector<CoolingDevice> cooling;
    if (!getTemperatures(&temperatures).isOk() || !getCoolingDevices(&cooling).isOk()) return false;
    monitor_ = std::thread(&Thermal::monitor, this);
    return true;
}

Thermal::~Thermal() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    if (monitor_.joinable()) monitor_.join();
}

ndk::ScopedAStatus Thermal::getTemperatures(std::vector<Temperature>* out) {
    out->clear();
    for (const auto& sensor : sensors_) {
        Temperature temperature;
        temperature.type = sensor.threshold.type;
        temperature.name = sensor.threshold.name;
        int64_t value;
        if (!readNumber(sensor.path + "/temp", &value)) return readError(sensor.path + "/temp");
        temperature.value = value / 1000.0f;
        temperature.throttlingStatus = ThrottlingSeverity::NONE;
        // Follow the zone's actual cooling devices, including the kernel's hysteresis.
        // Initial cooling is LIGHT, intermediate states MODERATE and maximum SEVERE.
        // Do not manufacture critical/shutdown policy from Samsung's hardware trips.
        for (const auto& link : sensor.coolingPaths) {
            int64_t state, maximum;
            if (!readNumber(link + "/cur_state", &state) ||
                !readNumber(link + "/max_state", &maximum) || state < 0 || state > maximum) {
                return readError(link);
            }
            auto severity = ThrottlingSeverity::NONE;
            if (state > 0) {
                severity = state >= maximum ? ThrottlingSeverity::SEVERE
                         : state == 1 ? ThrottlingSeverity::LIGHT : ThrottlingSeverity::MODERATE;
            }
            temperature.throttlingStatus = std::max(temperature.throttlingStatus, severity);
        }
        out->push_back(temperature);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::getTemperaturesWithType(TemperatureType type,
                                                    std::vector<Temperature>* out) {
    auto status = getTemperatures(out);
    filter(type, out);
    return status;
}

ndk::ScopedAStatus Thermal::getTemperatureThresholds(std::vector<TemperatureThreshold>* out) {
    out->clear();
    for (const auto& sensor : sensors_) out->push_back(sensor.threshold);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::getTemperatureThresholdsWithType(
        TemperatureType type, std::vector<TemperatureThreshold>* out) {
    auto status = getTemperatureThresholds(out);
    filter(type, out);
    return status;
}

ndk::ScopedAStatus Thermal::getCoolingDevices(std::vector<CoolingDevice>* out) {
    out->clear();
    for (const auto& cooler : coolers_) {
        CoolingDevice device = cooler.device;
        int64_t maximum;
        if (!readNumber(cooler.path + "/cur_state", &device.value) ||
            !readNumber(cooler.path + "/max_state", &maximum) ||
            device.value < 0 || device.value > maximum) return readError(cooler.path);
        out->push_back(device);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::getCoolingDevicesWithType(CoolingType type,
                                                     std::vector<CoolingDevice>* out) {
    auto status = getCoolingDevices(out);
    filter(type, out);
    return status;
}

template <class Callback, class Type>
ndk::ScopedAStatus Thermal::addCallback(std::vector<Registration<Callback, Type>>& list,
                                       const std::shared_ptr<Callback>& callback,
                                       std::optional<Type> type) {
    if (!callback) {
        return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                EX_ILLEGAL_ARGUMENT, "Callback is null");
    }
    std::lock_guard lock(mutex_);
    for (const auto& entry : list) {
        if (entry.callback->asBinder() == callback->asBinder()) {
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                    EX_ILLEGAL_ARGUMENT, "Callback is already registered");
        }
    }
    list.push_back({callback, type});
    return ndk::ScopedAStatus::ok();
}

template <class Callback, class Type>
ndk::ScopedAStatus Thermal::removeCallback(std::vector<Registration<Callback, Type>>& list,
                                          const std::shared_ptr<Callback>& callback) {
    if (!callback) {
        return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                EX_ILLEGAL_ARGUMENT, "Callback is null");
    }
    std::lock_guard lock(mutex_);
    auto entry = std::find_if(list.begin(), list.end(), [&](const auto& item) {
        return item.callback->asBinder() == callback->asBinder();
    });
    if (entry == list.end()) {
        return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                EX_ILLEGAL_ARGUMENT, "Callback is not registered");
    }
    list.erase(entry);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Thermal::registerThermalChangedCallback(
        const std::shared_ptr<IThermalChangedCallback>& callback) {
    return addCallback(thermalCallbacks_, callback, std::optional<TemperatureType>{});
}
ndk::ScopedAStatus Thermal::registerThermalChangedCallbackWithType(
        const std::shared_ptr<IThermalChangedCallback>& callback, TemperatureType type) {
    return addCallback(thermalCallbacks_, callback, std::optional{type});
}
ndk::ScopedAStatus Thermal::unregisterThermalChangedCallback(
        const std::shared_ptr<IThermalChangedCallback>& callback) {
    return removeCallback(thermalCallbacks_, callback);
}
ndk::ScopedAStatus Thermal::registerCoolingDeviceChangedCallbackWithType(
        const std::shared_ptr<ICoolingDeviceChangedCallback>& callback, CoolingType type) {
    return addCallback(coolingCallbacks_, callback, std::optional{type});
}
ndk::ScopedAStatus Thermal::unregisterCoolingDeviceChangedCallback(
        const std::shared_ptr<ICoolingDeviceChangedCallback>& callback) {
    return removeCallback(coolingCallbacks_, callback);
}

void Thermal::monitor() {
    while (true) {
        std::unique_lock lock(mutex_);
        if (condition_.wait_for(lock, std::chrono::seconds(2), [this] { return stopping_; })) return;
        const auto thermalCallbacks = thermalCallbacks_;
        const auto coolingCallbacks = coolingCallbacks_;
        lock.unlock();
        if (thermalCallbacks.empty() && coolingCallbacks.empty()) continue;

        std::vector<Temperature> temperatures;
        if (getTemperatures(&temperatures).isOk()) {
            for (size_t i = 0; i < temperatures.size(); ++i) {
                for (const auto& entry : thermalCallbacks) {
                    if (!AIBinder_isAlive(entry.callback->asBinder().get())) {
                        removeCallback(thermalCallbacks_, entry.callback);
                        continue;
                    }
                    if (entry.type && *entry.type != temperatures[i].type) continue;
                    const auto& temperature = temperatures[i];
                    const auto previous = entry.previous->find(temperature.name);
                    const auto severity = static_cast<int64_t>(temperature.throttlingStatus);
                    if (previous != entry.previous->end() && previous->second == severity) continue;
                    auto status = entry.callback->notifyThrottling(temperature);
                    if (status.isOk()) (*entry.previous)[temperature.name] = severity;
                    if (status.getStatus() == STATUS_DEAD_OBJECT) {
                        removeCallback(thermalCallbacks_, entry.callback);
                    }
                }
            }
        } else {
            LOG(WARNING) << "Failed to read thermal sensors";
        }
        std::vector<CoolingDevice> cooling;
        if (getCoolingDevices(&cooling).isOk()) {
            for (size_t i = 0; i < cooling.size(); ++i) {
                for (const auto& entry : coolingCallbacks) {
                    if (!AIBinder_isAlive(entry.callback->asBinder().get())) {
                        removeCallback(coolingCallbacks_, entry.callback);
                        continue;
                    }
                    if (entry.type && *entry.type != cooling[i].type) continue;
                    const auto& device = cooling[i];
                    const auto previous = entry.previous->find(device.name);
                    if (previous != entry.previous->end() && previous->second == device.value) continue;
                    auto status = entry.callback->notifyCoolingDeviceChanged(device);
                    if (status.isOk()) (*entry.previous)[device.name] = device.value;
                    if (status.getStatus() == STATUS_DEAD_OBJECT) {
                        removeCallback(coolingCallbacks_, entry.callback);
                    }
                }
            }
        }
    }
}

}  // namespace aidl::android::hardware::thermal

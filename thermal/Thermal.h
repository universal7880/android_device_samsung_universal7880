/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 The LineageOS Project
 */

#pragma once

#include <aidl/android/hardware/thermal/BnThermal.h>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace aidl::android::hardware::thermal {

class Thermal : public BnThermal {
  public:
    ~Thermal() override;
    bool initialize();

    ndk::ScopedAStatus getTemperatures(std::vector<Temperature>* out) override;
    ndk::ScopedAStatus getTemperaturesWithType(TemperatureType type,
                                              std::vector<Temperature>* out) override;
    ndk::ScopedAStatus getTemperatureThresholds(std::vector<TemperatureThreshold>* out) override;
    ndk::ScopedAStatus getTemperatureThresholdsWithType(
            TemperatureType type, std::vector<TemperatureThreshold>* out) override;
    ndk::ScopedAStatus getCoolingDevices(std::vector<CoolingDevice>* out) override;
    ndk::ScopedAStatus getCoolingDevicesWithType(CoolingType type,
                                               std::vector<CoolingDevice>* out) override;
    ndk::ScopedAStatus registerThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& callback) override;
    ndk::ScopedAStatus registerThermalChangedCallbackWithType(
            const std::shared_ptr<IThermalChangedCallback>& callback, TemperatureType type) override;
    ndk::ScopedAStatus unregisterThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& callback) override;
    ndk::ScopedAStatus registerCoolingDeviceChangedCallbackWithType(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& callback, CoolingType type) override;
    ndk::ScopedAStatus unregisterCoolingDeviceChangedCallback(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& callback) override;

  private:
    struct Sensor {
        std::string path;
        std::vector<std::string> coolingPaths;
        TemperatureThreshold threshold;
    };
    struct Cooler {
        std::string path;
        CoolingDevice device;
    };
    template <class Callback, class Type>
    struct Registration {
        std::shared_ptr<Callback> callback;
        std::optional<Type> type;
        std::shared_ptr<std::map<std::string, int64_t>> previous =
                std::make_shared<std::map<std::string, int64_t>>();
    };
    template <class Callback, class Type>
    ndk::ScopedAStatus addCallback(std::vector<Registration<Callback, Type>>& list,
                                   const std::shared_ptr<Callback>& callback,
                                   std::optional<Type> type);
    template <class Callback, class Type>
    ndk::ScopedAStatus removeCallback(std::vector<Registration<Callback, Type>>& list,
                                      const std::shared_ptr<Callback>& callback);
    void monitor();

    std::vector<Sensor> sensors_;
    std::vector<Cooler> coolers_;
    std::vector<Registration<IThermalChangedCallback, TemperatureType>> thermalCallbacks_;
    std::vector<Registration<ICoolingDeviceChangedCallback, CoolingType>> coolingCallbacks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
    std::thread monitor_;
};

}  // namespace aidl::android::hardware::thermal

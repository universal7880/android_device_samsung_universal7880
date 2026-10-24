/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 The LineageOS Project
 */
#pragma once

#include <aidl/android/hardware/thermal/CoolingDevice.h>
#include <aidl/android/hardware/thermal/Temperature.h>
#include <aidl/android/hardware/thermal/TemperatureThreshold.h>
#include <android-base/unique_fd.h>

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace aidl::android::hardware::thermal {

// Board-temperature policy, separate from the die TMU governor.
class ApThermalPolicy {
  public:
    bool initialize();
    bool enabled() const { return enabled_; }
    bool update();
    bool getTemperature(Temperature* out) const;
    CoolingDevice getCoolingDevice() const;
    const TemperatureThreshold& getThreshold() const { return threshold_; }

  private:
    bool setCpuLimit(size_t level);
    void setUnavailable();

    bool enabled_ = false;
    std::string path_;
    int firstTrip_ = 0;
    int skinOffset_ = 0;
    std::array<::android::base::unique_fd, 2> cpuRequests_;
    std::deque<int> samples_;
    int64_t lastSampleMs_ = 0;
    size_t level_ = 0;
    int appliedLevel_ = -1;
    TemperatureThreshold threshold_;

    mutable std::mutex mutex_;
    bool available_ = false;
    Temperature temperature_;
    CoolingDevice coolingDevice_;
};

}  // namespace aidl::android::hardware::thermal

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 The LineageOS Project
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace aidl::android::hardware::thermal::policy {

constexpr int kTripStep = 20;
constexpr int kReleaseGap = 20;

// Temperatures use deci-Celsius; retain the current level inside hysteresis.
inline size_t nextLevel(int temperature, int firstTrip, size_t current, size_t count) {
    size_t level = std::min(current, count - 1);
    while (level + 1 < count && temperature >= firstTrip + static_cast<int>(level) * kTripStep) {
        ++level;
    }
    while (level > 0 &&
           temperature < firstTrip + static_cast<int>(level - 1) * kTripStep - kReleaseGap) {
        --level;
    }
    return level;
}

// A720SKSU5CUJ2 CHG+DISPLAY policy: AP * BAT, with the ScreenOff override.
// The duplicate 70% step at AP 45 C does not change the requested current.
constexpr std::array<int, 5> kChargingLevels = {100, 70, 50, 30, 10};
constexpr std::array<int, 4> kChargingTrips = {300, 470, 490, 510};
constexpr int kChargingBatteryTrip = 300;

inline size_t nextChargingLevel(int ap, int battery, bool screenOn, size_t current) {
    if (!screenOn) return 0;
    size_t level = std::min(current, kChargingTrips.size());
    while (level < kChargingTrips.size() && ap >= kChargingTrips[level] &&
           battery >= kChargingBatteryTrip) {
        ++level;
    }
    while (level > 0 && (ap < kChargingTrips[level - 1] - kReleaseGap ||
                        battery < kChargingBatteryTrip - kReleaseGap)) {
        --level;
    }
    return level;
}

}  // namespace aidl::android::hardware::thermal::policy

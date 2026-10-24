/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 The LineageOS Project
 */
#pragma once

#include <algorithm>
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

}  // namespace aidl::android::hardware::thermal::policy

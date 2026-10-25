/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 The LineageOS Project
 */
#include "../Policy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using aidl::android::hardware::thermal::policy::nextLevel;
using aidl::android::hardware::thermal::policy::nextChargingLevel;
int main() {
    for (int first : {420, 430}) {
        assert(nextLevel(first - 1, first, 0, 7) == 0);
        for (size_t level = 1; level < 7; ++level) {
            const int trip = first + static_cast<int>(level - 1) * 20;
            assert(nextLevel(trip, first, 0, 7) == level);
            assert(nextLevel(trip - 20, first, level, 7) == level);
            assert(nextLevel(trip - 21, first, level, 7) == level - 1);
        }
        assert(nextLevel(900, first, 0, 7) == 6);
        assert(nextLevel(-200, first, 6, 7) == 0);
        const int gpuFirst = first + 20;
        assert(nextLevel(gpuFirst - 1, gpuFirst, 0, 4) == 0);
        for (size_t level = 1; level < 4; ++level) {
            const int trip = gpuFirst + static_cast<int>(level - 1) * 20;
            assert(nextLevel(trip, gpuFirst, 0, 4) == level);
            assert(nextLevel(trip - 20, gpuFirst, level, 4) == level);
            assert(nextLevel(trip - 21, gpuFirst, level, 4) == level - 1);
        }
        assert(nextLevel(900, gpuFirst, 0, 4) == 3);
        assert(nextLevel(-200, gpuFirst, 3, 4) == 0);
    }
    for (int displayTrip : {460, 470}) {
        assert(nextLevel(displayTrip - 1, displayTrip, 0, 2) == 0);
        assert(nextLevel(displayTrip, displayTrip, 0, 2) == 1);
        assert(nextLevel(displayTrip - 20, displayTrip, 1, 2) == 1);
        assert(nextLevel(displayTrip - 21, displayTrip, 1, 2) == 0);
        assert(nextLevel(900, displayTrip, 0, 2) == 1);
    }
    // AP and battery conditions must both hold. The display override also covers AOD.
    assert(nextChargingLevel(900, 299, true, 0) == 0);
    assert(nextChargingLevel(299, 500, true, 0) == 0);
    assert(nextChargingLevel(300, 300, true, 0) == 1);
    assert(nextChargingLevel(450, 300, true, 0) == 1);
    for (size_t level = 1; level <= 4; ++level) {
        const auto trip = aidl::android::hardware::thermal::policy::kChargingTrips[level - 1];
        assert(nextChargingLevel(trip, 300, true, 0) == level);
        assert(nextChargingLevel(trip - 20, 280, true, level) == level);
        assert(nextChargingLevel(trip - 21, 300, true, level) == level - 1);
        assert(nextChargingLevel(900, 279, true, level) == 0);
        assert(nextChargingLevel(900, 500, false, level) == 0);
    }
    assert(nextChargingLevel(900, 900, true, 0) == 4);
    assert(nextChargingLevel(-200, -200, true, 4) == 0);
    assert(nextChargingLevel(300, 300, true, 4) == 1);
    std::cout << "Charging AP/BAT gates, display override and hysteresis: PASS\n";
    std::cout << "A5/A7 CPU/GPU thresholds, hysteresis and extreme-temperature recovery: PASS\n";
}

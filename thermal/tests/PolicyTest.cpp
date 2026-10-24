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
    std::cout << "A5/A7 CPU/GPU thresholds, hysteresis and extreme-temperature recovery: PASS\n";
}

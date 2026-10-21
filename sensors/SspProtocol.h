/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ssp {

struct Sample {
    int64_t timestamp = 0;
    std::array<float, 16> values{};
    uint64_t steps = 0;
    int8_t accuracy = 3;
    bool flush = false;
};

size_t payloadSize(int handle);
bool decode(int handle, const uint8_t* record, size_t size, float scale, Sample* sample);

}  // namespace ssp

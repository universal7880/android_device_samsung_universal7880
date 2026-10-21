/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../SspProtocol.h"

#define CHECK(condition)                                                 \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            std::abort();                                                \
        }                                                                \
    } while (false)

namespace {
template <typename T>
void put(std::vector<uint8_t>& record, size_t offset, T value) {
    std::memcpy(record.data() + offset, &value, sizeof(value));
}

std::vector<uint8_t> packet(int handle, int64_t timestamp = 1234567890123LL) {
    std::vector<uint8_t> record(ssp::payloadSize(handle) + sizeof(timestamp));
    put(record, ssp::payloadSize(handle), timestamp);
    return record;
}

bool near(float left, float right) { return std::fabs(left - right) < 0.0001f; }
}  // namespace

int main() {
    ssp::Sample sample;
    auto accel = packet(1);
    put<int16_t>(accel, 0, 4096);
    put<int16_t>(accel, 2, -4096);
    put<int16_t>(accel, 4, -32768);
    CHECK(ssp::decode(1, accel.data(), accel.size(), 9.80665f / 4096, &sample));
    CHECK(near(sample.values[0], 9.80665f));
    CHECK(near(sample.values[1], -9.80665f));
    CHECK(near(sample.values[2], -78.4532f));
    CHECK(sample.timestamp == 1234567890123LL && !sample.flush);

    auto mag = packet(2);
    put<int16_t>(mag, 0, -100);
    mag[6] = 2;
    CHECK(ssp::decode(2, mag.data(), mag.size(), .06f, &sample));
    CHECK(near(sample.values[0], -6) && sample.accuracy == 2);
    CHECK(sample.timestamp == 1234567890123LL);  // Unaligned timestamp at byte 7.

    for (int handle : {14, 16}) {
        auto uncal = packet(handle);
        for (int i = 0; i < 6; ++i) put<int16_t>(uncal, i * 2, -30 + i);
        CHECK(ssp::decode(handle, uncal.data(), uncal.size(), .5f, &sample));
        for (int i = 0; i < 6; ++i) CHECK(near(sample.values[i], (-30 + i) * .5f));
    }

    auto pressure = packet(6);
    put<int32_t>(pressure, 0, 1013 * 4096 + 123);
    put<int16_t>(pressure, 4, -20);
    put<int32_t>(pressure, 6, 123);  // Packed calibration offset, not temperature.
    CHECK(ssp::decode(6, pressure.data(), pressure.size(), 1, &sample));
    CHECK(near(sample.values[0], 1013));

    auto proximity = packet(8);
    CHECK(ssp::decode(8, proximity.data(), proximity.size(), 1, &sample));
    CHECK(sample.values[0] == 8);
    proximity[0] = 1;
    CHECK(ssp::decode(8, proximity.data(), proximity.size(), 1, &sample));
    CHECK(sample.values[0] == 0);

    auto steps = packet(19);
    put<uint32_t>(steps, 0, 12);
    put<uint64_t>(steps, 4, 0x123456789abcdef0ULL);
    CHECK(ssp::decode(19, steps.data(), steps.size(), 1, &sample));
    CHECK(sample.steps == 0x123456789abcdef0ULL);

    auto context = packet(32, 0);
    for (size_t i = 0; i < 64; ++i) context[i] = i;
    CHECK(ssp::decode(32, context.data(), context.size(), 1, &sample));
    CHECK(!sample.flush && std::memcmp(sample.values.data(), context.data(), 64) == 0);

    auto cct = packet(34);
    put<uint32_t>(cct, 0, 54321);
    put<int32_t>(cct, 4, 6500);
    CHECK(ssp::decode(34, cct.data(), cct.size(), 1, &sample));
    CHECK(sample.values[0] == 54321 && sample.values[1] == 6500 && sample.values[2] == 54321);

    for (int handle : {1, 2, 4, 5, 6, 8, 14, 16, 17, 18, 19, 22, 25, 31, 32, 34}) {
        auto flush = packet(handle, 0);
        CHECK(ssp::decode(handle, flush.data(), flush.size(), 1, &sample) && sample.flush);
        CHECK(!ssp::decode(handle, flush.data(), flush.size() - 1, 1, &sample));
        CHECK(!ssp::decode(handle, flush.data(), flush.size() + 1, 1, &sample));
        put<int64_t>(flush, ssp::payloadSize(handle), -1);
        CHECK(!ssp::decode(handle, flush.data(), flush.size(), 1, &sample));
    }
    auto zeroGyro = packet(4);
    CHECK(ssp::decode(4, zeroGyro.data(), zeroGyro.size(), 1, &sample) && !sample.flush);
    CHECK(!ssp::decode(3, accel.data(), accel.size(), 1, &sample));
    CHECK(!ssp::decode(1, nullptr, accel.size(), 1, &sample));
    CHECK(!ssp::decode(1, accel.data(), accel.size(), 1, nullptr));
    std::puts("SSP packet tests passed");
}

/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "SspProtocol.h"

#include <cstring>

namespace ssp {
namespace {
template <typename T>
T read(const uint8_t* data) {
    T value;
    // SSP records are packed little-endian; both supported targets are little-endian.
    std::memcpy(&value, data, sizeof(value));
    return value;
}
}  // namespace

size_t payloadSize(int handle) {
    switch (handle) {
        case 1:
        case 4:
        case 31:
            return 6;
        case 2:
            return 7;
        case 5:
            return 4;
        case 6:
            return 14;
        case 8:
        case 17:
        case 18:
        case 22:
        case 25:
            return 1;
        case 14:
        case 16:
        case 19:
            return 12;
        case 32:
            return 64;
        case 34:
            return 8;
        default:
            return 0;
    }
}

bool decode(int handle, const uint8_t* record, size_t size, float scale, Sample* out) {
    const size_t payload = payloadSize(handle);
    if (!record || !out || !payload || size != payload + sizeof(int64_t)) return false;
    *out = {};
    out->accuracy = 3;
    out->timestamp = read<int64_t>(record + payload);
    if (out->timestamp < 0) return false;
    if (out->timestamp == 0) {
        // report_meta_data() emits an all-zero record on the sensor's own IIO stream.
        bool empty = true;
        for (size_t i = 0; i < payload; ++i) empty &= record[i] == 0;
        if (empty) {
            out->flush = true;
            return true;
        }
        // The context stream also carries untimestamped MCU reset notifications.
        if (handle != 32) return false;
    }
    switch (handle) {
        case 1:
        case 2:
        case 4:
        case 14:
        case 16:
        case 31:
            for (size_t i = 0; i < ((handle == 14 || handle == 16) ? 6U : 3U); ++i)
                out->values[i] = read<int16_t>(record + i * 2) * scale;
            if (handle == 2) out->accuracy = record[6];
            break;
        case 5:
            out->values[0] = read<uint32_t>(record);
            break;
        case 6:
            out->values[0] =
                (static_cast<double>(read<int32_t>(record)) - read<int32_t>(record + 6)) / 4096.0;
            break;
        case 8:
            out->values[0] = record[0] ? 0.0f : 8.0f;
            break;
        case 19:
            out->steps = read<uint64_t>(record + 4);
            break;
        case 32:
            // Preserve the private SContext byte payload, not a numeric float conversion.
            std::memcpy(out->values.data(), record, payload);
            break;
        case 34:
            out->values[0] = read<uint32_t>(record);
            out->values[1] = read<int32_t>(record + 4);
            out->values[2] = out->values[0];
            break;
        default:
            out->values[0] = record[0];
            break;
    }
    return true;
}
}  // namespace ssp

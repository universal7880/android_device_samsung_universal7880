/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 The LineageOS Project */
#include "container.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Packed PM3_01 layout from NXP tfa98xx_parameters.h. Avoid unaligned loads. */
static unsigned get16(const uint8_t *data) {
    return data[0] | ((unsigned)data[1] << 8);
}

static uint32_t get32(const uint8_t *data) {
    return get16(data) | ((uint32_t)get16(data + 2) << 16);
}

static bool inside(size_t offset, size_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

static bool valid_string(const uint8_t *data, size_t size, uint32_t descriptor) {
    size_t offset = descriptor & 0xffffff;
    return (descriptor >> 24) == 3 && offset < size &&
           memchr(data + offset, 0, size - offset) != NULL;
}

static bool valid_file(const uint8_t *data, size_t size, size_t offset) {
    if (!inside(offset, 8, size) || !valid_string(data, size, get32(data + offset))) {
        return false;
    }
    size_t length = get32(data + offset + 4);
    /* Every supported embedded file starts with the 36-byte generic header. */
    return length >= 36 && inside(offset + 8, length, size) &&
           get16(data + offset + 14) == length;
}

static bool valid_profile(const uint8_t *data, size_t size, size_t offset) {
    if (!inside(offset, 8, size)) return false;
    unsigned count = data[offset];
    if (!count || get16(data + offset + 2) != 0x1234 ||
        !valid_string(data, size, get32(data + offset + 4)) ||
        !inside(offset + 8, (count - 1) * 4, size)) {
        return false;
    }
    for (unsigned i = 0; i + 1 < count; ++i) {
        uint32_t descriptor = get32(data + offset + 8 + i * 4);
        /* This device's HQ profile contains file descriptors, not nested lists. */
        if ((descriptor >> 24) != 4 ||
            !valid_file(data, size, descriptor & 0xffffff)) return false;
    }
    return true;
}

int tfa_container_validate(const void *buffer, size_t size) {
    const uint8_t *data = buffer;
    if (!data || size < 54 || size > 256 * 1024 || memcmp(data, "PM3_01", 6) ||
        get32(data + 6) != size || get16(data + 40) != 1 ||
        get16(data + 42) != 1 || get16(data + 44) != 0) return -EINVAL;

    uint32_t descriptor = get32(data + 46);
    size_t offset = descriptor & 0xffffff;
    if ((descriptor >> 24) != 0 || !inside(offset, 12, size)) return -EINVAL;
    unsigned count = data[offset];
    if (!count || data[offset + 2] != 0x34 ||
        !valid_string(data, size, get32(data + offset + 8)) ||
        !inside(offset + 12, count * 4, size)) return -EINVAL;

    unsigned profiles = 0;
    for (unsigned i = 0; i < count; ++i) {
        descriptor = get32(data + offset + 12 + i * 4);
        size_t entry = descriptor & 0xffffff;
        switch (descriptor >> 24) {
            case 1:
                if (++profiles != 1 || !valid_profile(data, size, entry)) return -EINVAL;
                break;
            case 4:
            case 5:
                if (!valid_file(data, size, entry)) return -EINVAL;
                break;
            case 16:
                if (!inside(entry, 4, size)) return -EINVAL;
                break;
            default: return -EINVAL;
        }
    }
    return profiles == 1 ? 0 : -EINVAL;
}

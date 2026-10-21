/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <cstring>
#include <string>

#include <android-base/logging.h>
#include <android-base/properties.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include "vendor_init.h"

namespace {
struct Variant {
    const char* bootloaderPrefix;
    const char* device;
};

constexpr Variant variants[] = {
    {"A520", "a5y17lte"},
    {"A720", "a7y17lte"},
};

void overrideProperty(const char* name, const char* value) {
    auto* property = const_cast<prop_info*>(__system_property_find(name));
    if (property) {
        __system_property_update(property, value, std::strlen(value));
    } else {
        __system_property_add(name, std::strlen(name), value, std::strlen(value));
    }
}
}  // namespace

void vendor_load_properties() {
    const auto bootloader = android::base::GetProperty("ro.bootloader", "");
    const auto device = android::base::GetProperty("ro.product.device",
            android::base::GetProperty("ro.product.vendor.device",
                    android::base::GetProperty("ro.product.system.device", "")));
    const Variant* selected = nullptr;
    for (const auto& variant : variants) {
        if (bootloader.compare(0, std::strlen(variant.bootloaderPrefix),
                               variant.bootloaderPrefix) == 0) {
            selected = &variant;
            break;
        }
    }
    // Recovery or an empty bootloader property can still use the build's device identity.
    if (!selected) {
        for (const auto& variant : variants) {
            if (device == variant.device) {
                selected = &variant;
                break;
            }
        }
    }
    if (!selected) {
        LOG(ERROR) << "Cannot identify universal7880 variant: " << bootloader;
        return;
    }

    LOG(INFO) << "Using " << selected->device << " configuration";
    overrideProperty("ro.vendor.universal7880.device", selected->device);

    if (bootloader.size() >= 5 &&
        bootloader.compare(0, 4, selected->bootloaderPrefix) == 0) {
        const auto model = "SM-" + bootloader.substr(0, 5);
        for (const auto* partition : {"", "vendor.", "odm.", "system.", "system_ext.", "product."}) {
            const auto property = std::string("ro.product.") + partition + "model";
            overrideProperty(property.c_str(), model.c_str());
        }
    }


    if (bootloader.compare(0, 5, "A520K") == 0 ||
        bootloader.compare(0, 5, "A520L") == 0 ||
        bootloader.compare(0, 5, "A520S") == 0 ||
        bootloader.compare(0, 5, "A720S") == 0) {
        // SystemConfig loads the Korean FMRadio component override for this SKU.
        overrideProperty("ro.boot.product.vendor.sku", "kor");

        // Korean variants have a single SIM slot.
        overrideProperty("ro.multisim.simslotcount", "1");
        overrideProperty("ro.vendor.multisim.simslotcount", "1");
    }
}

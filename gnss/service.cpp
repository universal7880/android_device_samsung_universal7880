/*
 * Copyright (C) 2017 The Android Open Source Project
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "android.hardware.gnss@1.0-service"

#include <android/hardware/gnss/1.0/IGnss.h>
#include <binder/ProcessState.h>
#include <hidl/LegacySupport.h>

using android::hardware::defaultPassthroughServiceImplementation;
using android::hardware::gnss::V1_0::IGnss;

int main() {
    // The NDK sensor bridge talks to the framework AIDL SensorManager.
    // GNSS HIDL requests use the separate HwBinder transport.
    android::ProcessState::initWithDriver("/dev/binder");
    return defaultPassthroughServiceImplementation<IGnss>();
}

/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "FmRadioService"
#include <aidl/vendor/lineage/hardware/fmradio/BnFmRadio.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include <cstdlib>

#include "Si47xx.h"

using aidl::vendor::lineage::hardware::fmradio::BnFmRadio;

namespace {
class FmRadio : public BnFmRadio {
  public:
    ndk::ScopedAStatus open(bool* result) override {
        *result = radio.open();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus close(bool* result) override {
        *result = radio.close();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus powerUp(float mhz, bool* result) override {
        *result = radio.powerUp(mhz);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus powerDown(bool* result) override {
        *result = radio.powerDown();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus tune(float mhz, bool* result) override {
        *result = radio.tune(mhz);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus seek(float mhz, bool up, float* result) override {
        *result = radio.seek(mhz, up);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus scan(std::vector<int32_t>* result) override {
        auto channels = radio.scan();
        result->assign(channels.begin(), channels.end());
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus stopScan(bool* result) override {
        *result = radio.stopScan();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus mute(bool enabled, int32_t* result) override {
        *result = radio.mute(enabled);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus setRds(bool enabled, bool* result) override {
        *result = radio.setRds(enabled);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus readRds(int32_t* result) override {
        *result = radio.readRds();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus programName(std::string* result) override {
        *result = radio.programName();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus radioText(std::string* result) override {
        *result = radio.radioText();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus activeAf(int32_t* result) override {
        *result = radio.activeAf();
        return ndk::ScopedAStatus::ok();
    }

  private:
    Si47xx radio;
};
}  // namespace

int main() {
    // Seeking blocks; another Binder thread must remain available for cancellation.
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto radio = ndk::SharedRefBase::make<FmRadio>();
    const std::string instance = std::string(FmRadio::descriptor) + "/default";
    binder_status_t status = AServiceManager_addService(radio->asBinder().get(), instance.c_str());
    if (status != STATUS_OK) {
        ALOGE("Cannot register %s: %d", instance.c_str(), status);
        return EXIT_FAILURE;
    }
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}

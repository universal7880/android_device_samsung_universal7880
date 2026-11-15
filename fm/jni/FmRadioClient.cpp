/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "FmRadioClient"
#include "FmRadioClient.h"

#include <aidl/vendor/lineage/hardware/fmradio/BnFmRadioClient.h>
#include <aidl/vendor/lineage/hardware/fmradio/IFmRadio.h>
#include <android/binder_manager.h>
#include <log/log.h>

#include <mutex>

using aidl::vendor::lineage::hardware::fmradio::BnFmRadioClient;
using aidl::vendor::lineage::hardware::fmradio::IFmRadio;

namespace {
std::mutex serviceMutex;
std::shared_ptr<IFmRadio> service;
const auto clientToken = ndk::SharedRefBase::make<BnFmRadioClient>();

std::shared_ptr<IFmRadio> getService() {
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (!service || !AIBinder_isAlive(service->asBinder().get())) {
        const std::string instance = std::string(IFmRadio::descriptor) + "/default";
        service = IFmRadio::fromBinder(ndk::SpAIBinder(AServiceManager_checkService(instance.c_str())));
    }
    return service;
}

template <typename Result, typename Method, typename... Args>
Result call(Result fallback, Method method, Args... args) {
    auto radio = getService();
    if (!radio) return fallback;
    Result result{};
    auto status = (radio.get()->*method)(args..., &result);
    if (!status.isOk()) {
        ALOGE("FM transaction failed: %s", status.getDescription().c_str());
        std::lock_guard<std::mutex> lock(serviceMutex);
        if (service == radio) service.reset();
        return fallback;
    }
    return result;
}
}  // namespace

bool FmRadioClient::open() {
    return call(false, &IFmRadio::open, clientToken);
}

#ifdef FM_RADIO_VENDOR_AUDIO
int FmRadioClient::openAudio() {
    return call(ndk::ScopedFileDescriptor{}, &IFmRadio::openAudio).release();
}
#endif

bool FmRadioClient::close() {
    return call(false, &IFmRadio::close);
}

bool FmRadioClient::powerUp(float mhz) {
    return call(false, &IFmRadio::powerUp, mhz);
}

bool FmRadioClient::powerDown() {
    return call(false, &IFmRadio::powerDown);
}

bool FmRadioClient::tune(float mhz) {
    return call(false, &IFmRadio::tune, mhz);
}

float FmRadioClient::seek(float mhz, bool up) {
    return call(0.0f, &IFmRadio::seek, mhz, up);
}

std::vector<int16_t> FmRadioClient::scan() {
    auto channels = call(std::vector<int32_t>{}, &IFmRadio::scan);
    return {channels.begin(), channels.end()};
}

bool FmRadioClient::stopScan() {
    return call(false, &IFmRadio::stopScan);
}

int32_t FmRadioClient::mute(bool enabled) {
    return call(int32_t{-1}, &IFmRadio::mute, enabled);
}

bool FmRadioClient::setRds(bool enabled) {
    return call(false, &IFmRadio::setRds, enabled);
}

int32_t FmRadioClient::readRds() {
    return call(int32_t{0}, &IFmRadio::readRds);
}

std::string FmRadioClient::programName() {
    return call(std::string{}, &IFmRadio::programName);
}

std::string FmRadioClient::radioText() {
    return call(std::string{}, &IFmRadio::radioText);
}

int32_t FmRadioClient::activeAf() {
    return call(int32_t{0}, &IFmRadio::activeAf);
}

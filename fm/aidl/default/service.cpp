/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "FmRadioService"
#include <aidl/vendor/lineage/hardware/fmradio/BnFmRadio.h>
#include <android-base/cmsg.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <cutils/sockets.h>
#include <fcntl.h>
#include <log/log.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>

#include "Si47xx.h"

using aidl::vendor::lineage::hardware::fmradio::BnFmRadio;
using aidl::vendor::lineage::hardware::fmradio::IFmRadioClient;

namespace {
class FmRadio : public BnFmRadio {
  public:
    FmRadio() : deathRecipient(AIBinder_DeathRecipient_new(clientDied)) {
        AIBinder_DeathRecipient_setOnUnlinked(deathRecipient.get(), clientUnlinked);
    }

    ndk::ScopedAStatus open(const std::shared_ptr<IFmRadioClient>& client, bool* result) override {
        std::lock_guard<std::mutex> lock(sessionMutex);
        *result = false;
        if (!client) return ndk::ScopedAStatus::ok();
        auto binder = client->asBinder();
        if (clientBinder.get()) {
            *result = isSessionOwner() && clientBinder.get() == binder.get();
            return ndk::ScopedAStatus::ok();
        }
        if (!radio.open()) return ndk::ScopedAStatus::ok();

        auto* cookie = new DeathCookie{ref<FmRadio>(), ++generation};
        if (AIBinder_linkToDeath(binder.get(), deathRecipient.get(), cookie) != STATUS_OK) {
            // onUnlinked owns the cookie, including when linkToDeath fails.
            radio.close();
            return ndk::ScopedAStatus::ok();
        }
        clientBinder = std::move(binder);
        deathCookie = cookie;
        ownerUid = AIBinder_getCallingUid();
        ownerPid = AIBinder_getCallingPid();
        *result = true;
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus close(bool* result) override {
        std::lock_guard<std::mutex> lock(sessionMutex);
        *result = false;
        if (!isSessionOwner()) return ndk::ScopedAStatus::ok();
        stopAudio();
        ++generation;
        if (clientBinder.get()) {
            AIBinder_unlinkToDeath(clientBinder.get(), deathRecipient.get(), deathCookie);
            clientBinder = nullptr;
            deathCookie = nullptr;
        }
        *result = radio.close();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus powerUp(float mhz, bool* result) override {
        *result = radio.powerUp(mhz);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus powerDown(bool* result) override {
        std::lock_guard<std::mutex> lock(sessionMutex);
        *result = false;
        if (!isSessionOwner()) return ndk::ScopedAStatus::ok();
        stopAudio();
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

    ndk::ScopedAStatus openAudio(ndk::ScopedFileDescriptor* result) override {
        std::lock_guard<std::mutex> lock(sessionMutex);
        if (!isSessionOwner()) return ndk::ScopedAStatus::ok();
        stopAudio();

        ndk::ScopedFileDescriptor control(socket_local_client(
                "universal7880_fm_audio", ANDROID_SOCKET_NAMESPACE_RESERVED,
                SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK));
        int pipeFds[2];
        if (control.get() < 0 || pipe2(pipeFds, O_CLOEXEC) != 0) {
            ALOGE("Cannot open FM audio stream: %s", strerror(errno));
            return ndk::ScopedAStatus::ok();
        }
        ndk::ScopedFileDescriptor reader(pipeFds[0]);
        ndk::ScopedFileDescriptor writer(pipeFds[1]);
        if (fcntl(writer.get(), F_SETFL, O_NONBLOCK) < 0 ||
            android::base::SendFileDescriptors(control.get(), "F", 1, writer.get()) != 1) {
            ALOGE("Cannot hand off FM audio pipe: %s", strerror(errno));
            return ndk::ScopedAStatus::ok();
        }
        audioStream = std::move(control);
        *result = std::move(reader);
        return ndk::ScopedAStatus::ok();
    }

  private:
    bool isSessionOwner() const {
        return clientBinder.get() && ownerUid == AIBinder_getCallingUid() &&
               ownerPid == AIBinder_getCallingPid();
    }

    void stopAudio() {
        if (audioStream.get() < 0) return;
        shutdown(audioStream.get(), SHUT_RDWR);
        audioStream.set(-1);
    }

    struct DeathCookie {
        std::weak_ptr<FmRadio> service;
        uint64_t generation;
    };

    static void clientDied(void* data) {
        auto* cookie = static_cast<DeathCookie*>(data);
        auto service = cookie->service.lock();
        if (!service) return;
        std::lock_guard<std::mutex> lock(service->sessionMutex);
        // A delayed notification must not close a replacement session.
        if (cookie->generation != service->generation) return;
        service->stopAudio();
        ++service->generation;
        service->clientBinder = nullptr;
        service->deathCookie = nullptr;
        if (!service->radio.close()) ALOGE("Cannot close tuner after client death");
    }

    static void clientUnlinked(void* data) {
        delete static_cast<DeathCookie*>(data);
    }

    Si47xx radio;
    std::mutex sessionMutex;
    ndk::SpAIBinder clientBinder;
    ndk::ScopedFileDescriptor audioStream;
    uid_t ownerUid = 0;
    pid_t ownerPid = 0;
    DeathCookie* deathCookie = nullptr;
    uint64_t generation = 0;
    ndk::ScopedAIBinder_DeathRecipient deathRecipient;
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

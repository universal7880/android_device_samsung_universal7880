// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "WidevineAidl"

#include <aidl/android/hardware/drm/BnDrmFactory.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <dlfcn.h>
#include <log/log.h>

#include "LegacyDrm.h"

namespace widevine {
namespace {

using CreateDrmFactory = android::DrmFactory* (*)();

constexpr drm::Uuid kWidevineUuid{{0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce, 0xa3, 0xc8, 0x27,
                                   0xdc, 0xd5, 0x1d, 0x21, 0xed}};

class Factory final : public drm::BnDrmFactory {
   public:
    Factory(CreateDrmFactory drmFactory, decltype(&::createCryptoFactory) cryptoFactory)
        : drmFactory_(drmFactory), cryptoFactory_(cryptoFactory) {}

    ScopedAStatus createDrmPlugin(const drm::Uuid& uuid, const std::string&,
                                  std::shared_ptr<drm::IDrmPlugin>* result) override {
        std::unique_ptr<android::DrmFactory> factory(drmFactory_());
        if (!factory || uuid != kWidevineUuid ||
            !factory->isCryptoSchemeSupported(uuid.uuid.data())) {
            return error(drm::Status::ERROR_DRM_CANNOT_HANDLE);
        }
        android::DrmPlugin* plugin = nullptr;
        auto status = factory->createDrmPlugin(uuid.uuid.data(), &plugin);
        if (status != android::OK) return legacyStatus(status);
        if (!plugin) return error(drm::Status::ERROR_DRM_CANNOT_HANDLE);
        *result = wrapDrm(plugin);
        return ScopedAStatus::ok();
    }

    ScopedAStatus createCryptoPlugin(const drm::Uuid& uuid, const Bytes& initData,
                                     std::shared_ptr<drm::ICryptoPlugin>* result) override {
        std::unique_ptr<android::CryptoFactory> factory(cryptoFactory_());
        if (!factory || uuid != kWidevineUuid ||
            !factory->isCryptoSchemeSupported(uuid.uuid.data())) {
            return error(drm::Status::ERROR_DRM_CANNOT_HANDLE);
        }
        android::CryptoPlugin* plugin = nullptr;
        auto status =
            factory->createPlugin(uuid.uuid.data(), initData.data(), initData.size(), &plugin);
        if (status != android::OK) return legacyStatus(status);
        if (!plugin) return error(drm::Status::ERROR_DRM_CANNOT_HANDLE);
        *result = wrapCrypto(plugin);
        return ScopedAStatus::ok();
    }

    ScopedAStatus getSupportedCryptoSchemes(drm::CryptoSchemes* result) override {
        std::unique_ptr<android::DrmFactory> factory(drmFactory_());
        if (!factory || !factory->isCryptoSchemeSupported(kWidevineUuid.uuid.data())) {
            return ScopedAStatus::ok();
        }
        android::DrmPlugin* rawPlugin = nullptr;
        auto status = factory->createDrmPlugin(kWidevineUuid.uuid.data(), &rawPlugin);
        if (status != android::OK) return legacyStatus(status);
        std::unique_ptr<android::DrmPlugin> plugin(rawPlugin);
        if (!plugin) return error(drm::Status::ERROR_DRM_CANNOT_HANDLE);

        const auto maxLevel = readSecurityLevel(*plugin);
        const auto minLevel = selectSecurityLevel(*plugin, drm::SecurityLevel::SW_SECURE_CRYPTO)
                                  ? drm::SecurityLevel::SW_SECURE_CRYPTO
                                  : maxLevel;
        result->uuids = {kWidevineUuid};
        if (maxLevel == drm::SecurityLevel::UNKNOWN) return ScopedAStatus::ok();

        // AIDL enumerates MIME types whereas the legacy ABI queries each MIME individually.
        for (const char* mime : {"video/mp4", "audio/mp4", "video/webm", "audio/webm",
                                 "video/x-matroska", "audio/x-matroska", "cenc", "webm"}) {
            if (factory->isContentTypeSupported(android::String8(mime))) {
                result->mimeTypes.push_back({mime, minLevel, maxLevel});
            }
        }
        return ScopedAStatus::ok();
    }

   private:
    const CreateDrmFactory drmFactory_;
    const decltype(&::createCryptoFactory) cryptoFactory_;
};

}  // namespace
}  // namespace widevine

int main() {
    // Both factories and their plugins keep code and vtables in this library.
    void* engine = dlopen("/vendor/lib/mediadrm/libwvdrmengine.so", RTLD_NOW | RTLD_LOCAL);
    if (!engine) {
        ALOGE("Cannot load the Widevine engine: %s", dlerror());
        return 1;
    }
    auto drmFactory =
        reinterpret_cast<widevine::CreateDrmFactory>(dlsym(engine, "createDrmFactory"));
    auto cryptoFactory =
        reinterpret_cast<decltype(&::createCryptoFactory)>(dlsym(engine, "createCryptoFactory"));
    if (!drmFactory || !cryptoFactory) {
        ALOGE("Widevine engine does not export both factory functions");
        return 1;
    }

    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto service = ndk::SharedRefBase::make<widevine::Factory>(drmFactory, cryptoFactory);
    const std::string instance = std::string(widevine::drm::IDrmFactory::descriptor) + "/widevine";
    if (AServiceManager_addService(service->asBinder().get(), instance.c_str()) != STATUS_OK) {
        ALOGE("Cannot register %s", instance.c_str());
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return 1;
}

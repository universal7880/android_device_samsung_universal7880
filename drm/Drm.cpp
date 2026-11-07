// SPDX-License-Identifier: Apache-2.0
#include <aidl/android/hardware/drm/BnDrmPlugin.h>
#include <media/stagefright/MediaErrors.h>

#include <map>
#include <mutex>

#include "LegacyDrm.h"

namespace widevine {

using drm::SecurityLevel;
using drm::Status;
using Legacy = android::DrmPlugin;
using LegacyBytes = android::Vector<uint8_t>;

ScopedAStatus error(Status status) {
    return ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(status));
}

ScopedAStatus legacyStatus(android::status_t status) {
    switch (status) {
        case android::OK:
            return ScopedAStatus::ok();
        case android::BAD_VALUE:
            return error(Status::BAD_VALUE);
        case android::ERROR_DRM_NO_LICENSE:
            return error(Status::ERROR_DRM_NO_LICENSE);
        case android::ERROR_DRM_LICENSE_EXPIRED:
            return error(Status::ERROR_DRM_LICENSE_EXPIRED);
        case android::ERROR_DRM_SESSION_NOT_OPENED:
            return error(Status::ERROR_DRM_SESSION_NOT_OPENED);
        case android::ERROR_UNSUPPORTED:
        case android::ERROR_DRM_CANNOT_HANDLE:
            return error(Status::ERROR_DRM_CANNOT_HANDLE);
        case android::ERROR_DRM_TAMPER_DETECTED:
        case android::ERROR_DRM_INVALID_STATE:
            return error(Status::ERROR_DRM_INVALID_STATE);
        case android::ERROR_DRM_NOT_PROVISIONED:
            return error(Status::ERROR_DRM_NOT_PROVISIONED);
        case android::ERROR_DRM_RESOURCE_BUSY:
            return error(Status::ERROR_DRM_RESOURCE_BUSY);
        case android::ERROR_DRM_INSUFFICIENT_OUTPUT_PROTECTION:
            return error(Status::ERROR_DRM_INSUFFICIENT_OUTPUT_PROTECTION);
        case android::ERROR_DRM_DEVICE_REVOKED:
            return error(Status::ERROR_DRM_DEVICE_REVOKED);
        case android::ERROR_DRM_DECRYPT:
            return error(Status::ERROR_DRM_DECRYPT);
        case android::ERROR_DRM_INSUFFICIENT_SECURITY:
            return error(Status::ERROR_DRM_INSUFFICIENT_SECURITY);
        case android::ERROR_DRM_FRAME_TOO_LARGE:
            return error(Status::ERROR_DRM_FRAME_TOO_LARGE);
        case android::ERROR_DRM_SESSION_LOST_STATE:
            return error(Status::ERROR_DRM_SESSION_LOST_STATE);
        default:
            return error(Status::ERROR_DRM_UNKNOWN);
    }
}

SecurityLevel readSecurityLevel(const Legacy& plugin) {
    android::String8 level;
    if (plugin.getPropertyString(android::String8("securityLevel"), level) == android::OK) {
        if (level == android::String8("L1")) return SecurityLevel::HW_SECURE_ALL;
        if (level == android::String8("L3")) return SecurityLevel::SW_SECURE_CRYPTO;
    }
    return SecurityLevel::UNKNOWN;
}

bool selectSecurityLevel(Legacy& plugin, SecurityLevel level) {
    if (level == readSecurityLevel(plugin)) return true;
    const char* value;
    switch (level) {
        case SecurityLevel::HW_SECURE_ALL:
            value = "L1";
            break;
        case SecurityLevel::SW_SECURE_CRYPTO:
            value = "L3";
            break;
        default:
            return false;
    }
    return plugin.setPropertyString(android::String8("securityLevel"), android::String8(value)) ==
               android::OK &&
           readSecurityLevel(plugin) == level;
}

namespace {

ScopedAStatus unsupported() { return error(Status::ERROR_DRM_CANNOT_HANDLE); }

ScopedAStatus byteResult(android::status_t status, const LegacyBytes& bytes, Bytes* result) {
    if (status == android::OK) *result = aidlBytes(bytes);
    return legacyStatus(status);
}

drm::KeyRequestType requestType(Legacy::KeyRequestType type) {
    switch (type) {
        case Legacy::kKeyRequestType_Initial:
            return drm::KeyRequestType::INITIAL;
        case Legacy::kKeyRequestType_Renewal:
            return drm::KeyRequestType::RENEWAL;
        case Legacy::kKeyRequestType_Release:
            return drm::KeyRequestType::RELEASE;
        case Legacy::kKeyRequestType_None:
            return drm::KeyRequestType::NONE;
        case Legacy::kKeyRequestType_Update:
            return drm::KeyRequestType::UPDATE;
        default:
            return drm::KeyRequestType::UNKNOWN;
    }
}

class Listener final : public android::DrmPluginListener {
   public:
    explicit Listener(std::shared_ptr<drm::IDrmPluginListener> listener)
        : listener_(std::move(listener)) {}

    void sendEvent(Legacy::EventType type, int, const LegacyBytes* session,
                   const LegacyBytes* data) override {
        const auto id = session ? aidlBytes(*session) : Bytes{};
        switch (type) {
            case Legacy::kDrmPluginEventProvisionRequired:
            case Legacy::kDrmPluginEventKeyNeeded:
            case Legacy::kDrmPluginEventKeyExpired:
            case Legacy::kDrmPluginEventVendorDefined:
            case Legacy::kDrmPluginEventSessionReclaimed:
                listener_->onEvent(static_cast<drm::EventType>(type - 1), id,
                                   data ? aidlBytes(*data) : Bytes{});
                break;
            case Legacy::kDrmPluginEventSessionLostState:
                listener_->onSessionLostState(id);
                break;
            default:
                break;
        }
    }

    void sendExpirationUpdate(const LegacyBytes* session, int64_t expiry) override {
        listener_->onExpirationUpdate(session ? aidlBytes(*session) : Bytes{}, expiry);
    }

    void sendKeysChange(const LegacyBytes* session,
                        const android::Vector<Legacy::KeyStatus>* statuses, bool usable) override {
        std::vector<drm::KeyStatus> keys;
        if (statuses) {
            for (const auto& status : *statuses) {
                keys.push_back(
                    {aidlBytes(status.mKeyId), static_cast<drm::KeyStatusType>(status.mType)});
            }
        }
        listener_->onKeysChange(session ? aidlBytes(*session) : Bytes{}, keys, usable);
    }

   private:
    const std::shared_ptr<drm::IDrmPluginListener> listener_;
};

class Drm final : public drm::BnDrmPlugin {
   public:
    explicit Drm(Legacy* plugin) : plugin_(plugin), nativeLevel_(readSecurityLevel(*plugin)) {}
    ~Drm() override { plugin_->setListener(nullptr); }

    ScopedAStatus openSession(SecurityLevel requested, Bytes* result) override {
        std::lock_guard lock(mutex_);
        // The legacy engine selects L1 or L3 for the plugin, rather than per session.
        if (requested < SecurityLevel::SW_SECURE_CRYPTO || requested > SecurityLevel::DEFAULT ||
            (requested != SecurityLevel::DEFAULT && requested > nativeLevel_)) {
            return error(Status::ERROR_DRM_INSUFFICIENT_SECURITY);
        }
        if (requested != SecurityLevel::DEFAULT) {
            const auto level = requested == SecurityLevel::HW_SECURE_ALL
                                   ? SecurityLevel::HW_SECURE_ALL
                                   : SecurityLevel::SW_SECURE_CRYPTO;
            if ((!sessions_.empty() && readSecurityLevel(*plugin_) != level) ||
                !selectSecurityLevel(*plugin_, level)) {
                return error(Status::ERROR_DRM_INSUFFICIENT_SECURITY);
            }
        }
        LegacyBytes session;
        const auto status = plugin_->openSession(session);
        if (status != android::OK) return legacyStatus(status);
        const auto level = readSecurityLevel(*plugin_);
        if (level == SecurityLevel::UNKNOWN ||
            (requested != SecurityLevel::DEFAULT && level > requested)) {
            plugin_->closeSession(session);
            return error(Status::ERROR_DRM_INSUFFICIENT_SECURITY);
        }
        *result = aidlBytes(session);
        sessions_[*result] = level;
        return ScopedAStatus::ok();
    }

    ScopedAStatus closeSession(const Bytes& session) override {
        std::lock_guard lock(mutex_);
        auto status = plugin_->closeSession(legacyBytes(session));
        if (status == android::OK) sessions_.erase(session);
        return legacyStatus(status);
    }

    ScopedAStatus getSecurityLevel(const Bytes& session, SecurityLevel* result) override {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(session);
        if (it == sessions_.end()) return error(Status::ERROR_DRM_SESSION_NOT_OPENED);
        *result = it->second;
        return ScopedAStatus::ok();
    }

    ScopedAStatus getKeyRequest(const Bytes& scope, const Bytes& initData, const std::string& mime,
                                drm::KeyType type, const std::vector<drm::KeyValue>& options,
                                drm::KeyRequest* result) override {
        if (type != drm::KeyType::OFFLINE && type != drm::KeyType::STREAMING &&
            type != drm::KeyType::RELEASE) {
            return error(Status::BAD_VALUE);
        }
        android::KeyedVector<android::String8, android::String8> parameters;
        for (const auto& option : options) {
            parameters.add(android::String8(option.key.c_str()),
                           android::String8(option.value.c_str()));
        }
        LegacyBytes request;
        android::String8 url;
        auto kind = Legacy::kKeyRequestType_Unknown;
        std::lock_guard lock(mutex_);
        auto status = plugin_->getKeyRequest(
            legacyBytes(scope), legacyBytes(initData), android::String8(mime.c_str()),
            static_cast<Legacy::KeyType>(type), parameters, request, url, &kind);
        if (status != android::OK) return legacyStatus(status);
        result->request = aidlBytes(request);
        result->requestType = requestType(kind);
        result->defaultUrl = url.c_str();
        return ScopedAStatus::ok();
    }

    ScopedAStatus provideKeyResponse(const Bytes& scope, const Bytes& response,
                                     drm::KeySetId* result) override {
        LegacyBytes id;
        std::lock_guard lock(mutex_);
        return byteResult(
            plugin_->provideKeyResponse(legacyBytes(scope), legacyBytes(response), id), id,
            &result->keySetId);
    }

    ScopedAStatus removeKeys(const Bytes& session) override {
        std::lock_guard lock(mutex_);
        return legacyStatus(plugin_->removeKeys(legacyBytes(session)));
    }

    ScopedAStatus restoreKeys(const Bytes& session, const drm::KeySetId& id) override {
        std::lock_guard lock(mutex_);
        return legacyStatus(plugin_->restoreKeys(legacyBytes(session), legacyBytes(id.keySetId)));
    }

    ScopedAStatus queryKeyStatus(const Bytes& session,
                                 std::vector<drm::KeyValue>* result) override {
        android::KeyedVector<android::String8, android::String8> info;
        std::lock_guard lock(mutex_);
        auto status = plugin_->queryKeyStatus(legacyBytes(session), info);
        if (status != android::OK) return legacyStatus(status);
        for (size_t i = 0; i < info.size(); ++i) {
            result->push_back({info.keyAt(i).c_str(), info.valueAt(i).c_str()});
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus getProvisionRequest(const std::string& type, const std::string& authority,
                                      drm::ProvisionRequest* result) override {
        LegacyBytes request;
        android::String8 url;
        std::lock_guard lock(mutex_);
        auto status = plugin_->getProvisionRequest(
            android::String8(type.c_str()), android::String8(authority.c_str()), request, url);
        if (status != android::OK) return legacyStatus(status);
        result->request = aidlBytes(request);
        result->defaultUrl = url.c_str();
        return ScopedAStatus::ok();
    }

    ScopedAStatus provideProvisionResponse(const Bytes& response,
                                           drm::ProvideProvisionResponseResult* result) override {
        LegacyBytes certificate;
        LegacyBytes key;
        std::lock_guard lock(mutex_);
        auto status = plugin_->provideProvisionResponse(legacyBytes(response), certificate, key);
        if (status != android::OK) return legacyStatus(status);
        result->certificate = aidlBytes(certificate);
        result->wrappedKey = aidlBytes(key);
        return ScopedAStatus::ok();
    }

    ScopedAStatus getPropertyString(const std::string& name, std::string* result) override {
        android::String8 value;
        std::lock_guard lock(mutex_);
        auto status = plugin_->getPropertyString(android::String8(name.c_str()), value);
        if (status == android::OK) *result = value.c_str();
        return legacyStatus(status);
    }

    ScopedAStatus getPropertyByteArray(const std::string& name, Bytes* result) override {
        LegacyBytes value;
        std::lock_guard lock(mutex_);
        return byteResult(plugin_->getPropertyByteArray(android::String8(name.c_str()), value),
                          value, result);
    }

    ScopedAStatus setPropertyString(const std::string& name, const std::string& value) override {
        std::lock_guard lock(mutex_);
        return legacyStatus(plugin_->setPropertyString(android::String8(name.c_str()),
                                                       android::String8(value.c_str())));
    }

    ScopedAStatus setPropertyByteArray(const std::string& name, const Bytes& value) override {
        std::lock_guard lock(mutex_);
        return legacyStatus(
            plugin_->setPropertyByteArray(android::String8(name.c_str()), legacyBytes(value)));
    }

    ScopedAStatus setListener(const std::shared_ptr<drm::IDrmPluginListener>& listener) override {
        std::lock_guard lock(mutex_);
        android::sp<android::DrmPluginListener> adapter;
        if (listener) adapter = android::sp<Listener>::make(listener);
        return legacyStatus(plugin_->setListener(adapter));
    }

    ScopedAStatus getSecureStop(const drm::SecureStopId& id, drm::SecureStop* result) override {
        LegacyBytes data;
        std::lock_guard lock(mutex_);
        return byteResult(plugin_->getSecureStop(legacyBytes(id.secureStopId), data), data,
                          &result->opaqueData);
    }

    ScopedAStatus getSecureStops(std::vector<drm::SecureStop>* result) override {
        android::List<LegacyBytes> stops;
        std::lock_guard lock(mutex_);
        auto status = plugin_->getSecureStops(stops);
        if (status != android::OK) return legacyStatus(status);
        for (const auto& stop : stops) result->push_back({aidlBytes(stop)});
        return ScopedAStatus::ok();
    }

    ScopedAStatus releaseSecureStops(const drm::OpaqueData& release) override {
        std::lock_guard lock(mutex_);
        return legacyStatus(plugin_->releaseSecureStops(legacyBytes(release.opaqueData)));
    }

    ScopedAStatus releaseAllSecureStops() override {
        std::lock_guard lock(mutex_);
        return legacyStatus(plugin_->releaseAllSecureStops());
    }

    ScopedAStatus setCipherAlgorithm(const Bytes& session, const std::string& algorithm) override {
        std::lock_guard lock(mutex_);
        return legacyStatus(
            plugin_->setCipherAlgorithm(legacyBytes(session), android::String8(algorithm.c_str())));
    }

    ScopedAStatus setMacAlgorithm(const Bytes& session, const std::string& algorithm) override {
        std::lock_guard lock(mutex_);
        return legacyStatus(
            plugin_->setMacAlgorithm(legacyBytes(session), android::String8(algorithm.c_str())));
    }

    ScopedAStatus encrypt(const Bytes& session, const Bytes& key, const Bytes& input,
                          const Bytes& iv, Bytes* result) override {
        LegacyBytes output;
        std::lock_guard lock(mutex_);
        return byteResult(plugin_->encrypt(legacyBytes(session), legacyBytes(key),
                                           legacyBytes(input), legacyBytes(iv), output),
                          output, result);
    }

    ScopedAStatus decrypt(const Bytes& session, const Bytes& key, const Bytes& input,
                          const Bytes& iv, Bytes* result) override {
        LegacyBytes output;
        std::lock_guard lock(mutex_);
        return byteResult(plugin_->decrypt(legacyBytes(session), legacyBytes(key),
                                           legacyBytes(input), legacyBytes(iv), output),
                          output, result);
    }

    ScopedAStatus sign(const Bytes& session, const Bytes& key, const Bytes& message,
                       Bytes* result) override {
        LegacyBytes signature;
        std::lock_guard lock(mutex_);
        return byteResult(
            plugin_->sign(legacyBytes(session), legacyBytes(key), legacyBytes(message), signature),
            signature, result);
    }

    ScopedAStatus signRSA(const Bytes& session, const std::string& algorithm, const Bytes& message,
                          const Bytes& wrappedKey, Bytes* result) override {
        LegacyBytes signature;
        std::lock_guard lock(mutex_);
        return byteResult(
            plugin_->signRSA(legacyBytes(session), android::String8(algorithm.c_str()),
                             legacyBytes(message), legacyBytes(wrappedKey), signature),
            signature, result);
    }

    ScopedAStatus verify(const Bytes& session, const Bytes& key, const Bytes& message,
                         const Bytes& signature, bool* result) override {
        std::lock_guard lock(mutex_);
        return legacyStatus(plugin_->verify(legacyBytes(session), legacyBytes(key),
                                            legacyBytes(message), legacyBytes(signature), *result));
    }

    // These operations have no equivalent in the A720S legacy plugin ABI.
    ScopedAStatus getHdcpLevels(drm::HdcpLevels*) override { return unsupported(); }
    ScopedAStatus getMetrics(std::vector<drm::DrmMetricGroup>*) override { return unsupported(); }
    ScopedAStatus getNumberOfSessions(drm::NumberOfSessions*) override { return unsupported(); }
    ScopedAStatus getOfflineLicenseKeySetIds(std::vector<drm::KeySetId>*) override {
        return unsupported();
    }
    ScopedAStatus getOfflineLicenseState(const drm::KeySetId&, drm::OfflineLicenseState*) override {
        return unsupported();
    }
    ScopedAStatus getSecureStopIds(std::vector<drm::SecureStopId>*) override {
        return unsupported();
    }
    ScopedAStatus releaseSecureStop(const drm::SecureStopId&) override { return unsupported(); }
    ScopedAStatus removeAllSecureStops() override { return unsupported(); }
    ScopedAStatus removeOfflineLicense(const drm::KeySetId&) override { return unsupported(); }
    ScopedAStatus removeSecureStop(const drm::SecureStopId&) override { return unsupported(); }
    ScopedAStatus requiresSecureDecoder(const std::string&, SecurityLevel, bool*) override {
        return unsupported();
    }
    ScopedAStatus setPlaybackId(const Bytes&, const std::string&) override { return unsupported(); }

    ScopedAStatus getLogMessages(std::vector<drm::LogMessage>* result) override {
        result->clear();
        return ScopedAStatus::ok();
    }

   private:
    const std::unique_ptr<Legacy> plugin_;
    const SecurityLevel nativeLevel_;
    std::mutex mutex_;
    std::map<Bytes, SecurityLevel> sessions_;
};

}  // namespace

std::shared_ptr<drm::IDrmPlugin> wrapDrm(Legacy* plugin) {
    return ndk::SharedRefBase::make<Drm>(plugin);
}

}  // namespace widevine

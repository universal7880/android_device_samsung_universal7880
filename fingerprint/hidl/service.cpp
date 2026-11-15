// Copyright (C) 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0

#include <aidl/android/hardware/biometrics/fingerprint/BnSessionCallback.h>
#include <aidl/android/hardware/biometrics/fingerprint/IFingerprint.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/hardware/biometrics/fingerprint/2.1/IBiometricsFingerprint.h>
#include <endian.h>
#include <hardware/hw_auth_token.h>
#include <hidl/HidlTransportSupport.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace {

namespace aidl_fp = aidl::android::hardware::biometrics::fingerprint;
namespace hidl_fp = android::hardware::biometrics::fingerprint::V2_1;
using aidl::android::hardware::biometrics::common::ICancellationSignal;
using aidl::android::hardware::keymaster::HardwareAuthenticatorType;
using aidl::android::hardware::keymaster::HardwareAuthToken;
using android::sp;
using android::hardware::Return;
using android::hardware::hidl_array;
using android::hardware::hidl_string;
using android::hardware::hidl_vec;
using hidl_fp::FingerprintAcquiredInfo;
using hidl_fp::FingerprintError;
using hidl_fp::IBiometricsFingerprintClientCallback;
using hidl_fp::RequestStatus;
using ndk::ScopedAStatus;
using namespace std::chrono_literals;

constexpr uint64_t kDeviceId = 1;
constexpr size_t kTokenSize = 69;
static_assert(sizeof(hw_auth_token_t) == kTokenSize);

HardwareAuthToken decodeToken(const hw_auth_token_t& token) {
    HardwareAuthToken result;
    result.challenge = token.challenge;
    result.userId = token.user_id;
    result.authenticatorId = token.authenticator_id;
    result.authenticatorType = static_cast<HardwareAuthenticatorType>(be32toh(token.authenticator_type));
    result.timestamp.milliSeconds = be64toh(token.timestamp);
    result.mac.assign(token.hmac, token.hmac + sizeof(token.hmac));
    return result;
}

FingerprintError toHidlError(aidl_fp::Error error) {
    switch (error) {
        case aidl_fp::Error::HW_UNAVAILABLE: return FingerprintError::ERROR_HW_UNAVAILABLE;
        case aidl_fp::Error::TIMEOUT: return FingerprintError::ERROR_TIMEOUT;
        case aidl_fp::Error::NO_SPACE: return FingerprintError::ERROR_NO_SPACE;
        case aidl_fp::Error::CANCELED: return FingerprintError::ERROR_CANCELED;
        case aidl_fp::Error::UNABLE_TO_REMOVE: return FingerprintError::ERROR_UNABLE_TO_REMOVE;
        case aidl_fp::Error::VENDOR: return FingerprintError::ERROR_VENDOR;
        default: return FingerprintError::ERROR_UNABLE_TO_PROCESS;
    }
}

class SessionCallback : public aidl_fp::BnSessionCallback {
  public:
    enum class Reply { None, Challenge, Revoked, AuthenticatorId, Enumerated };

    SessionCallback(uint32_t group, sp<IBiometricsFingerprintClientCallback> client)
        : group_(group), client_(std::move(client)) {}

    void setClient(const sp<IBiometricsFingerprintClientCallback>& client) {
        std::lock_guard lock(mutex_);
        client_ = client;
    }

    template <typename Command>
    bool request(Reply reply, Command command) {
        {
            std::lock_guard lock(mutex_);
            pending_ = reply;
            failed_ = false;
        }
        const auto status = command();
        std::unique_lock lock(mutex_);
        const bool completed = status.isOk() &&
                ready_.wait_for(lock, 3s, [&] { return pending_ == Reply::None; }) && !failed_;
        pending_ = Reply::None;
        return completed;
    }

    int64_t value() {
        std::lock_guard lock(mutex_);
        return value_;
    }

    std::vector<int32_t> enrollments() {
        std::lock_guard lock(mutex_);
        return enrollments_;
    }

    void expectRemoval(const std::vector<int32_t>& ids) {
        std::lock_guard lock(mutex_);
        removing_ = ids;
    }

    ScopedAStatus onChallengeGenerated(int64_t challenge) override {
        return complete(Reply::Challenge, challenge);
    }

    ScopedAStatus onChallengeRevoked(int64_t challenge) override {
        return complete(Reply::Revoked, challenge);
    }

    ScopedAStatus onAuthenticatorIdRetrieved(int64_t id) override {
        return complete(Reply::AuthenticatorId, id);
    }

    ScopedAStatus onAcquired(aidl_fp::AcquiredInfo info, int32_t vendorCode) override {
        // AIDL places UNKNOWN before the shared GOOD through VENDOR values.
        if (info >= aidl_fp::AcquiredInfo::GOOD && info <= aidl_fp::AcquiredInfo::VENDOR) {
            client()->onAcquired(kDeviceId, static_cast<FingerprintAcquiredInfo>(
                    static_cast<int32_t>(info) - static_cast<int32_t>(aidl_fp::AcquiredInfo::GOOD)),
                    vendorCode);
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus onError(aidl_fp::Error error, int32_t vendorCode) override {
        {
            std::lock_guard lock(mutex_);
            if (pending_ != Reply::None) {
                failed_ = true;
                pending_ = Reply::None;
                ready_.notify_one();
            }
            removing_.clear();
        }
        client()->onError(kDeviceId, toHidlError(error), vendorCode);
        return ScopedAStatus::ok();
    }

    ScopedAStatus onEnrollmentProgress(int32_t id, int32_t remaining) override {
        client()->onEnrollResult(kDeviceId, id, group_, std::max(remaining, 0));
        return ScopedAStatus::ok();
    }

    ScopedAStatus onAuthenticationSucceeded(int32_t id, const HardwareAuthToken& token) override {
        hw_auth_token_t legacy{};
        if (id <= 0 || token.mac.size() != sizeof(legacy.hmac)) {
            return onError(aidl_fp::Error::UNABLE_TO_PROCESS, 0);
        }
        legacy.version = HW_AUTH_TOKEN_VERSION;
        legacy.challenge = token.challenge;
        legacy.user_id = token.userId;
        legacy.authenticator_id = token.authenticatorId;
        legacy.authenticator_type = htobe32(static_cast<uint32_t>(token.authenticatorType));
        legacy.timestamp = htobe64(token.timestamp.milliSeconds);
        std::copy(token.mac.begin(), token.mac.end(), legacy.hmac);
        hidl_vec<uint8_t> bytes;
        bytes.resize(sizeof(legacy));
        std::memcpy(bytes.data(), &legacy, sizeof(legacy));
        client()->onAuthenticated(kDeviceId, id, group_, bytes);
        return ScopedAStatus::ok();
    }

    ScopedAStatus onAuthenticationFailed() override {
        client()->onAuthenticated(kDeviceId, 0, group_, {});
        return ScopedAStatus::ok();
    }

    ScopedAStatus onLockoutTimed(int64_t) override {
        client()->onError(kDeviceId, FingerprintError::ERROR_LOCKOUT, 0);
        return ScopedAStatus::ok();
    }

    ScopedAStatus onLockoutPermanent() override {
        // The framework fingerprint error code for permanent lockout is 9.
        client()->onError(kDeviceId, static_cast<FingerprintError>(9), 0);
        return ScopedAStatus::ok();
    }

    ScopedAStatus onLockoutCleared() override { return ScopedAStatus::ok(); }
    ScopedAStatus onInteractionDetected() override { return ScopedAStatus::ok(); }
    ScopedAStatus onAuthenticatorIdInvalidated(int64_t) override { return ScopedAStatus::ok(); }
    ScopedAStatus onSessionClosed() override { return ScopedAStatus::ok(); }

    ScopedAStatus onEnrollmentsEnumerated(const std::vector<int32_t>& ids) override {
        std::vector<int32_t> valid;
        std::copy_if(ids.begin(), ids.end(), std::back_inserter(valid), [](int32_t id) { return id > 0; });
        {
            std::lock_guard lock(mutex_);
            if (pending_ == Reply::Enumerated) {
                enrollments_ = std::move(valid);
                pending_ = Reply::None;
                ready_.notify_one();
                return ScopedAStatus::ok();
            }
        }
        const auto recipient = client();
        if (valid.empty()) {
            recipient->onEnumerate(kDeviceId, 0, group_, 0);
        }
        for (size_t i = 0; i < valid.size(); ++i) {
            recipient->onEnumerate(kDeviceId, valid[i], group_, valid.size() - i - 1);
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus onEnrollmentsRemoved(const std::vector<int32_t>& ids) override {
        std::lock_guard lock(mutex_);
        for (int32_t id : ids) {
            const auto it = std::find(removing_.begin(), removing_.end(), id);
            if (it == removing_.end()) continue;
            removing_.erase(it);
            client_->onRemoved(kDeviceId, id, group_, removing_.size());
        }
        return ScopedAStatus::ok();
    }

  private:
    sp<IBiometricsFingerprintClientCallback> client() {
        std::lock_guard lock(mutex_);
        return client_;
    }

    ScopedAStatus complete(Reply reply, int64_t value) {
        std::lock_guard lock(mutex_);
        if (pending_ == reply) {
            value_ = value;
            pending_ = Reply::None;
            ready_.notify_one();
        }
        return ScopedAStatus::ok();
    }

    const uint32_t group_;
    sp<IBiometricsFingerprintClientCallback> client_;
    std::mutex mutex_;
    std::condition_variable ready_;
    Reply pending_ = Reply::None;
    bool failed_ = false;
    int64_t value_ = 0;
    std::vector<int32_t> enrollments_;
    std::vector<int32_t> removing_;
};

class ClientDeath : public android::hardware::hidl_death_recipient {
  public:
    void serviceDied(uint64_t, const android::wp<android::hidl::base::V1_0::IBase>&) override {
        // The Samsung HAL closes the AIDL session when its callback binder dies.
        std::_Exit(EXIT_SUCCESS);
    }
};

// HIDL requests run on one thread. AIDL callbacks use the separate Binder pool.
class FingerprintCompat : public hidl_fp::IBiometricsFingerprint {
  public:
    Return<uint64_t> setNotify(const sp<IBiometricsFingerprintClientCallback>& callback) override {
        if (!callback) return 0;
        if (client_ == callback) return kDeviceId;
        const auto linked = callback->linkToDeath(death_, 0);
        if (!linked.isOk() || !static_cast<bool>(linked)) return 0;
        if (client_) {
            const auto unlinked = client_->unlinkToDeath(death_);
            if (!unlinked.isOk()) LOG(WARNING) << "Cannot unlink the previous HIDL callback";
        }
        client_ = callback;
        // The framework can replace its callback after selecting the active group.
        if (callback_) callback_->setClient(callback);
        return kDeviceId;
    }

    Return<RequestStatus> setActiveGroup(uint32_t group, const hidl_string&) override {
        if (!client_ || group > std::numeric_limits<int32_t>::max()) {
            LOG(ERROR) << "Cannot set fingerprint group " << group << ": invalid group or no callback";
            return RequestStatus::SYS_EINVAL;
        }
        if (activeGroup(group)) return RequestStatus::SYS_OK;
        if (!closeSession()) {
            LOG(ERROR) << "Cannot close the previous fingerprint session";
            return RequestStatus::SYS_EIO;
        }

        const std::string instance = std::string(aidl_fp::IFingerprint::descriptor) + "/default";
        const auto hal = aidl_fp::IFingerprint::fromBinder(
                ndk::SpAIBinder(AServiceManager_checkService(instance.c_str())));
        if (!hal) {
            LOG(ERROR) << "Fingerprint AIDL service unavailable: " << instance;
            return RequestStatus::SYS_EIO;
        }
        std::vector<aidl_fp::SensorProps> sensors;
        const auto propsStatus = hal->getSensorProps(&sensors);
        if (!propsStatus.isOk() || sensors.size() != 1) {
            LOG(ERROR) << "Cannot select fingerprint sensor: " << propsStatus.getDescription()
                       << ", sensors=" << sensors.size();
            return RequestStatus::SYS_EIO;
        }
        auto callback = ndk::SharedRefBase::make<SessionCallback>(group, client_);
        std::shared_ptr<aidl_fp::ISession> session;
        // The AIDL HAL selects its own vendor storage path for this user.
        const auto sessionStatus =
                hal->createSession(sensors.front().commonProps.sensorId, group, callback, &session);
        if (!sessionStatus.isOk() || !session) {
            LOG(ERROR) << "Cannot create fingerprint session for group " << group << ": "
                       << sessionStatus.getDescription();
            return RequestStatus::SYS_EIO;
        }
        group_ = group;
        callback_ = std::move(callback);
        session_ = std::move(session);
        return RequestStatus::SYS_OK;
    }

    Return<uint64_t> preEnroll() override {
        if (!session_ || !callback_->request(SessionCallback::Reply::Challenge,
                                              [&] { return session_->generateChallenge(); })) {
            return 0;
        }
        challenge_ = callback_->value();
        return *challenge_;
    }

    Return<RequestStatus> postEnroll() override {
        if (!session_ || !challenge_) return RequestStatus::SYS_EINVAL;
        if (!callback_->request(SessionCallback::Reply::Revoked,
                                [&] { return session_->revokeChallenge(*challenge_); })) {
            return RequestStatus::SYS_EIO;
        }
        challenge_.reset();
        return RequestStatus::SYS_OK;
    }

    Return<RequestStatus> enroll(const hidl_array<uint8_t, kTokenSize>& bytes, uint32_t group,
                                 uint32_t /*timeoutSec*/) override {
        if (!activeGroup(group)) {
            LOG(ERROR) << "HIDL enrollment has no active AIDL session for group " << group;
            return reportError(RequestStatus::SYS_EINVAL, FingerprintError::ERROR_HW_UNAVAILABLE);
        }
        hw_auth_token_t token;
        std::memcpy(&token, bytes.data(), sizeof(token));
        if (token.version != HW_AUTH_TOKEN_VERSION) {
            LOG(ERROR) << "HIDL enrollment received an unsupported auth token version";
            return reportError(RequestStatus::SYS_EINVAL, FingerprintError::ERROR_UNABLE_TO_PROCESS);
        }
        cancellation_.reset();
        LOG(INFO) << "Forwarding HIDL enrollment to the AIDL HAL for group " << group;
        const auto status = session_->enroll(decodeToken(token), &cancellation_);
        if (!status.isOk()) {
            LOG(ERROR) << "AIDL fingerprint enrollment request failed: " << status.getDescription();
            return reportError(RequestStatus::SYS_EIO, FingerprintError::ERROR_HW_UNAVAILABLE);
        }
        return RequestStatus::SYS_OK;
    }

    Return<uint64_t> getAuthenticatorId() override {
        if (!session_ || !callback_->request(SessionCallback::Reply::AuthenticatorId,
                                              [&] { return session_->getAuthenticatorId(); })) {
            return 0;
        }
        return callback_->value();
    }

    Return<RequestStatus> authenticate(uint64_t operationId, uint32_t group) override {
        if (!activeGroup(group)) return RequestStatus::SYS_EINVAL;
        cancellation_.reset();
        return toRequestStatus(session_->authenticate(operationId, &cancellation_));
    }

    Return<RequestStatus> cancel() override {
        if (!cancellation_) {
            if (callback_) callback_->onError(aidl_fp::Error::CANCELED, 0);
            return RequestStatus::SYS_OK;
        }
        const auto signal = std::exchange(cancellation_, nullptr);
        return toRequestStatus(signal->cancel());
    }

    Return<RequestStatus> enumerate() override {
        if (!session_) {
            LOG(ERROR) << "HIDL enumeration has no active AIDL session";
            return reportError(RequestStatus::SYS_EINVAL, FingerprintError::ERROR_HW_UNAVAILABLE);
        }
        const auto status = toRequestStatus(session_->enumerateEnrollments());
        if (status != RequestStatus::SYS_OK) {
            return reportError(status, FingerprintError::ERROR_HW_UNAVAILABLE);
        }
        return status;
    }

    Return<RequestStatus> remove(uint32_t group, uint32_t id) override {
        if (!activeGroup(group) || id > std::numeric_limits<int32_t>::max()) {
            return RequestStatus::SYS_EINVAL;
        }
        std::vector<int32_t> ids;
        if (id != 0) {
            ids.push_back(id);
        } else {
            if (!callback_->request(SessionCallback::Reply::Enumerated,
                                    [&] { return session_->enumerateEnrollments(); })) {
                return RequestStatus::SYS_EIO;
            }
            ids = callback_->enrollments();
            if (ids.empty()) {
                client_->onRemoved(kDeviceId, 0, group, 0);
                return RequestStatus::SYS_OK;
            }
        }
        callback_->expectRemoval(ids);
        return toRequestStatus(session_->removeEnrollments(ids));
    }

  private:
    RequestStatus reportError(RequestStatus status, FingerprintError error) {
        // Some framework clients discard the synchronous HIDL request status.
        if (client_) {
            const auto delivered = client_->onError(kDeviceId, error, 0);
            if (!delivered.isOk()) {
                LOG(WARNING) << "Cannot report HIDL fingerprint error: " << delivered.description();
            }
        }
        return status;
    }

    static RequestStatus toRequestStatus(const ScopedAStatus& status) {
        if (status.isOk()) return RequestStatus::SYS_OK;
        LOG(ERROR) << "Fingerprint AIDL request failed: " << status.getDescription();
        return RequestStatus::SYS_EIO;
    }

    bool activeGroup(uint32_t group) const {
        return session_ && group_ == group && AIBinder_isAlive(session_->asBinder().get());
    }

    bool closeSession() {
        if (!session_) return true;
        if (AIBinder_isAlive(session_->asBinder().get())) {
            if (cancellation_) {
                // Cancel before switching users, while the callback still belongs to the old user.
                const auto status = cancellation_->cancel();
                if (!status.isOk() && status.getStatus() != STATUS_DEAD_OBJECT) return false;
            }
            const auto status = session_->close();
            if (!status.isOk() && status.getStatus() != STATUS_DEAD_OBJECT) return false;
        }
        cancellation_.reset();
        challenge_.reset();
        session_.reset();
        callback_.reset();
        return true;
    }

    const sp<ClientDeath> death_ = sp<ClientDeath>::make();
    sp<IBiometricsFingerprintClientCallback> client_;
    uint32_t group_ = 0;
    std::optional<int64_t> challenge_;
    std::shared_ptr<SessionCallback> callback_;
    std::shared_ptr<aidl_fp::ISession> session_;
    std::shared_ptr<ICancellationSignal> cancellation_;
};

}  // namespace

int main() {
    ABinderProcess_startThreadPool();
    android::hardware::configureRpcThreadpool(1, true);
    const auto service = sp<FingerprintCompat>::make();
    if (service->registerAsService() != android::OK) {
        LOG(ERROR) << "Cannot register the fingerprint HIDL adapter";
        return EXIT_FAILURE;
    }
    android::hardware::joinRpcThreadpool();
    return EXIT_FAILURE;
}

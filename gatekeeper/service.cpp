// SPDX-License-Identifier: Apache-2.0
#include <aidl/android/hardware/gatekeeper/BnGatekeeper.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <endian.h>
#include <gatekeeper/password_handle.h>
#include <hardware/hw_auth_token.h>

#include <cstring>
#include <mutex>

#include "Trustlet.h"

namespace {

using namespace aidl::android::hardware::gatekeeper;
using aidl::android::hardware::security::keymint::HardwareAuthenticatorType;
using ndk::ScopedAStatus;

class Gatekeeper final : public BnGatekeeper {
  public:
    ScopedAStatus enroll(int32_t uid, const std::vector<uint8_t>& handle,
                         const std::vector<uint8_t>& current, const std::vector<uint8_t>& desired,
                         GatekeeperEnrollResponse* response) override {
        if (uid < 0) return failure();
        std::lock_guard lock(mutex_);
        auto result = trustlet_.enroll(uid, handle, current, desired);
        if (result.timeoutMs > 0) {
            response->statusCode = ERROR_RETRY_TIMEOUT;
            response->timeoutMs = result.timeoutMs;
            return ScopedAStatus::ok();
        }
        ::gatekeeper::password_handle_t newHandle{};
        if (!result.success || result.data.size() != sizeof(newHandle)) return failure();
        memcpy(&newHandle, result.data.data(), sizeof(newHandle));
        response->statusCode = STATUS_OK;
        response->secureUserId = newHandle.user_id;
        response->data = std::move(result.data);
        return ScopedAStatus::ok();
    }

    ScopedAStatus verify(int32_t uid, int64_t challenge, const std::vector<uint8_t>& handle,
                         const std::vector<uint8_t>& password,
                         GatekeeperVerifyResponse* response) override {
        if (uid < 0) return failure();
        std::lock_guard lock(mutex_);
        auto result = trustlet_.verify(uid, challenge, handle, password);
        if (result.timeoutMs > 0) {
            response->statusCode = ERROR_RETRY_TIMEOUT;
            response->timeoutMs = result.timeoutMs;
            return ScopedAStatus::ok();
        }
        hw_auth_token_t token{};
        if (!result.success || result.data.size() != sizeof(token)) return failure();
        memcpy(&token, result.data.data(), sizeof(token));
        if (token.version != HW_AUTH_TOKEN_VERSION || token.challenge != uint64_t(challenge)) {
            return failure();
        }
        response->statusCode = result.reenroll ? int32_t{STATUS_REENROLL} : int32_t{STATUS_OK};
        auto& auth = response->hardwareAuthToken;
        auth.challenge = token.challenge;
        auth.userId = token.user_id;
        auth.authenticatorId = token.authenticator_id;
        auth.authenticatorType = static_cast<HardwareAuthenticatorType>(
                be32toh(token.authenticator_type));
        auth.timestamp.milliSeconds = be64toh(token.timestamp);
        auth.mac.assign(token.hmac, token.hmac + sizeof(token.hmac));
        return ScopedAStatus::ok();
    }

    ScopedAStatus deleteAllUsers() override {
        return ScopedAStatus::fromServiceSpecificError(ERROR_NOT_IMPLEMENTED);
    }

    ScopedAStatus deleteUser(int32_t) override {
        return ScopedAStatus::fromServiceSpecificError(ERROR_NOT_IMPLEMENTED);
    }

  private:
    static ScopedAStatus failure() {
        return ScopedAStatus::fromServiceSpecificError(ERROR_GENERAL_FAILURE);
    }

    std::mutex mutex_;
    samsung::gatekeeper::Trustlet trustlet_;
};

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    const auto gatekeeper = ndk::SharedRefBase::make<Gatekeeper>();
    const std::string instance = std::string(IGatekeeper::descriptor) + "/default";
    if (AServiceManager_addService(gatekeeper->asBinder().get(), instance.c_str()) != STATUS_OK) {
        return 1;
    }
    ABinderProcess_joinThreadPool();
    return 1;
}

/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <aidl/android/hardware/bluetooth/BnBluetoothHci.h>
#include <aidl/android/hardware/bluetooth/IBluetoothHciCallbacks.h>
#include <android/binder_auto_utils.h>

#include <mutex>

namespace universal7880 {
class BluetoothHci : public aidl::android::hardware::bluetooth::BnBluetoothHci {
   public:
    BluetoothHci();
    ~BluetoothHci() override;
    ndk::ScopedAStatus initialize(
        const std::shared_ptr<aidl::android::hardware::bluetooth::IBluetoothHciCallbacks>&) override;
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus sendHciCommand(const std::vector<uint8_t>&) override;
    ndk::ScopedAStatus sendAclData(const std::vector<uint8_t>&) override;
    ndk::ScopedAStatus sendScoData(const std::vector<uint8_t>&) override;
    ndk::ScopedAStatus sendIsoData(const std::vector<uint8_t>&) override;

   private:
    struct Session;
    struct DeathCookie;
    std::mutex mutex_;
    std::shared_ptr<Session> session_;
    bool closing_ = false;
    ndk::ScopedAIBinder_DeathRecipient death_;
    void closeSession(const std::shared_ptr<Session>& expected);
    ndk::ScopedAStatus send(uint8_t type, const std::vector<uint8_t>& packet);
    static void receive(std::shared_ptr<Session> session);
    static bool readExact(const std::shared_ptr<Session>&, uint8_t*, size_t);
    static void hardwareError(const std::shared_ptr<Session>&);
    static void clientDied(void*);
    static void unlinked(void*);
};
}  // namespace universal7880

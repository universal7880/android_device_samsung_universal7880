/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <com/dsi/ant/1.0/IAnt.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace universal7880 {
class Ant : public com::dsi::ant::V1_0::IAnt {
   public:
    ~Ant() override;
    android::hardware::Return<void> getProperties(getProperties_cb callback) override;
    android::hardware::Return<void> setCallbacks(
        const android::sp<com::dsi::ant::V1_0::IAntCallbacks>& callback) override;
    android::hardware::Return<void> translateStatus(int32_t status,
                                                 translateStatus_cb callback) override;
    android::hardware::Return<int32_t> enable() override;
    android::hardware::Return<int32_t> disable() override;
    android::hardware::Return<int32_t> sendDataMessage(
        const android::hardware::hidl_vec<uint8_t>& message) override;
    android::hardware::Return<int32_t> sendCommandMessage(
        const android::hardware::hidl_vec<uint8_t>& message) override;
    void clientDied(uint64_t cookie);

   private:
    class DeathRecipient;
    std::mutex state_mutex_, callback_mutex_;
    android::sp<com::dsi::ant::V1_0::IAntCallbacks> callback_;
    android::sp<android::hardware::hidl_death_recipient> death_;
    uint64_t generation_ = 0;
    int fd_ = -1, cancel_[2] = {-1, -1};
    std::thread reader_;
    std::atomic<bool> stopping_{false}, failed_{false};
    void stopLocked();
    void receive();
    void transportFailed(const char* cause);
    bool readExact(uint8_t* data, size_t size);
    int32_t send(uint8_t type, const android::hardware::hidl_vec<uint8_t>& message);
};
}  // namespace universal7880

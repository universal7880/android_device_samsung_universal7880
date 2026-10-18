/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "ant.universal7880"
#include "Ant.h"

#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>

#include "ant_controller.h"
#include "bluetooth_address.h"

namespace universal7880 {
using android::sp;
using android::hardware::hidl_vec;
using android::hardware::Return;
using android::hardware::Void;
using com::dsi::ant::V1_0::IAntCallbacks;
using com::dsi::ant::V1_0::ImplProps;
using com::dsi::ant::V1_0::OptionFlags;

class Ant::DeathRecipient : public android::hardware::hidl_death_recipient {
   public:
    explicit DeathRecipient(Ant* owner) : owner_(owner) {}
    void serviceDied(uint64_t cookie,
                     const android::wp<android::hidl::base::V1_0::IBase>&) override {
        sp<Ant> owner = owner_.promote();
        if (owner) owner->clientDied(cookie);
    }

   private:
    android::wp<Ant> owner_;
};

Ant::~Ant() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    stopLocked();
}

Return<void> Ant::getProperties(getProperties_cb callback) {
    ImplProps props;
    props.version = "Universal7880 ANT 1.0";
    props.options = static_cast<uint32_t>(OptionFlags::USE_ANT_FLOW_CONTROL);
    callback(props);
    return Void();
}

Return<void> Ant::translateStatus(int32_t status, translateStatus_cb callback) {
    callback(status < 0 && status >= -4095 ? strerror(-status)
             : status == 0                 ? "Success"
                                           : "Transport error");
    return Void();
}

Return<void> Ant::setCallbacks(const sp<IAntCallbacks>& callback) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    stopLocked();
    sp<IAntCallbacks> old;
    {
        std::lock_guard<std::mutex> cb_lock(callback_mutex_);
        old = callback_;
        callback_ = callback;
    }
    if (old && death_) {
        auto result = old->unlinkToDeath(death_);
        if (!result.isOk()) ALOGW("Cannot unlink the previous ANT client");
    }
    ++generation_;
    if (callback) {
        if (!death_) death_ = new DeathRecipient(this);
        auto result = callback->linkToDeath(death_, generation_);
        if (!result.isOk() || !static_cast<bool>(result)) {
            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
            callback_.clear();
        }
    }
    return Void();
}

void Ant::clientDied(uint64_t cookie) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (cookie != generation_) return;
    stopLocked();
    std::lock_guard<std::mutex> cb_lock(callback_mutex_);
    callback_.clear();
}

Return<int32_t> Ant::enable() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (fd_ >= 0) return failed_ ? -EIO : 0;
    {
        std::lock_guard<std::mutex> cb_lock(callback_mutex_);
        if (!callback_) return -ENOTCONN;
    }
    uint8_t address[6];
    if (!android::hardware::bluetooth::V1_0::implementation::BluetoothAddress::get_local_address(
            address))
        return -EIO;
    if (pipe2(cancel_, O_CLOEXEC | O_NONBLOCK) < 0) return -errno;
    fd_ = ant_controller_acquire(address);
    if (fd_ < 0) {
        close(cancel_[0]);
        close(cancel_[1]);
        cancel_[0] = cancel_[1] = -1;
        return -EIO;
    }
    stopping_ = failed_ = false;
    reader_ = std::thread(&Ant::receive, this);
    return 0;
}

void Ant::stopLocked() {
    if (fd_ < 0) return;
    stopping_ = true;
    const uint8_t stop = 1;
    while (write(cancel_[1], &stop, 1) < 0 && errno == EINTR) {
    }
    if (reader_.joinable()) reader_.join();
    ant_controller_release();
    fd_ = -1;
    close(cancel_[0]);
    close(cancel_[1]);
    cancel_[0] = cancel_[1] = -1;
}

Return<int32_t> Ant::disable() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    stopLocked();
    return 0;
}

bool Ant::readExact(uint8_t* data, size_t size) {
    size_t used = 0;
    while (used < size && !stopping_) {
        pollfd fds[] = {{cancel_[0], POLLIN, 0}, {fd_, POLLIN, 0}};
        int result = poll(fds, 2, -1);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || fds[0].revents) return false;
        if (!(fds[1].revents & POLLIN)) return false;
        ssize_t count = recv(fd_, data + used, size - used, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return false;
        used += count;
    }
    return used == size;
}

void Ant::receive() {
    while (!stopping_) {
        uint8_t header[2];
        if (!readExact(header, sizeof(header))) break;
        if ((header[0] != 0x0c && header[0] != 0x0e) || header[1] < 2) break;
        hidl_vec<uint8_t> message;
        message.resize(header[1]);
        if (!readExact(message.data(), message.size())) break;
        if (message[0] + 2u != message.size() || (message[1] == 0xc9 && message.size() < 3)) break;
        sp<IAntCallbacks> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = callback_;
        }
        if (callback) {
            auto result = callback->onMessageReceived(message);
            if (!result.isOk()) break;
        }
    }
    if (!stopping_) transportFailed("ANT controller transport failed");
}

void Ant::transportFailed(const char* cause) {
    if (failed_.exchange(true)) return;
    // A write failure must also wake a reader waiting for the next controller packet.
    const uint8_t stop = 1;
    while (write(cancel_[1], &stop, 1) < 0 && errno == EINTR) {
    }
    sp<IAntCallbacks> callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = callback_;
    }
    if (callback) {
        auto result = callback->onTransportDown(cause);
        if (!result.isOk()) ALOGW("Cannot notify the ANT client of transport failure");
    }
}

int32_t Ant::send(uint8_t type, const hidl_vec<uint8_t>& message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (fd_ < 0 || failed_) return -ENOTCONN;
    if (message.size() < 2 || message.size() > 255 || message[0] + 2u != message.size())
        return -EINVAL;
    std::array<uint8_t, 257> packet;
    packet[0] = type;
    packet[1] = message.size();
    memcpy(packet.data() + 2, message.data(), message.size());
    size_t used = 0, size = message.size() + 2;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (used < size) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0) break;
        pollfd fd{fd_, POLLOUT, 0};
        int result = poll(&fd, 1, remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        ssize_t count = ::send(fd_, packet.data() + used, size - used, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) break;
        used += count;
    }
    if (used == size) return 0;
    transportFailed("ANT controller write failed");
    return -EIO;
}

Return<int32_t> Ant::sendDataMessage(const hidl_vec<uint8_t>& message) {
    return send(0x0e, message);
}
Return<int32_t> Ant::sendCommandMessage(const hidl_vec<uint8_t>& message) {
    return send(0x0c, message);
}
}  // namespace universal7880

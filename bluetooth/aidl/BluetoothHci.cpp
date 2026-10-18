/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "bluetooth.universal7880"
#include "BluetoothHci.h"

#include <android/binder_ibinder.h>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <poll.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "bluetooth_address.h"
#include "bluetooth_controller.h"

namespace universal7880 {
using aidl::android::hardware::bluetooth::IBluetoothHciCallbacks;
using aidl::android::hardware::bluetooth::Status;
using ndk::ScopedAStatus;

struct BluetoothHci::Session {
    std::shared_ptr<IBluetoothHciCallbacks> callback;
    int fd = -1, cancel[2] = {-1, -1};
    void* death_cookie = nullptr;
    std::atomic<bool> stopping{false}, failed{false};
    std::thread reader;
    ~Session() {
        for (int owned : cancel)
            if (owned >= 0) ::close(owned);
    }
    void wake() const {
        const uint8_t byte = 1;
        while (write(cancel[1], &byte, 1) < 0 && errno == EINTR) {
        }
    }
};
struct BluetoothHci::DeathCookie {
    std::weak_ptr<BluetoothHci> owner;
    std::weak_ptr<Session> session;
};

BluetoothHci::BluetoothHci() : death_(AIBinder_DeathRecipient_new(clientDied)) {
    AIBinder_DeathRecipient_setOnUnlinked(death_.get(), unlinked);
}
BluetoothHci::~BluetoothHci() { closeSession(nullptr); }
void BluetoothHci::unlinked(void* cookie) { delete static_cast<DeathCookie*>(cookie); }
void BluetoothHci::clientDied(void* value) {
    auto* cookie = static_cast<DeathCookie*>(value);
    auto owner = cookie->owner.lock();
    auto session = cookie->session.lock();
    if (owner && session) owner->closeSession(session);
}

ScopedAStatus BluetoothHci::initialize(const std::shared_ptr<IBluetoothHciCallbacks>& callback) {
    if (!callback) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    auto session = std::make_shared<Session>();
    session->callback = callback;
    Status result = Status::UNABLE_TO_OPEN_INTERFACE;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_ || closing_) {
            result = Status::ALREADY_INITIALIZED;
        } else if (pipe2(session->cancel, O_CLOEXEC | O_NONBLOCK) == 0) {
            bool linked = true;
            if (AIBinder_isRemote(callback->asBinder().get())) {
                auto* cookie = new DeathCookie{ref<BluetoothHci>(), session};
                linked = AIBinder_linkToDeath(callback->asBinder().get(), death_.get(), cookie) ==
                         STATUS_OK;
                // onUnlinked owns the cookie even if linkToDeath fails.
                if (linked) session->death_cookie = cookie;
            }
            uint8_t address[6];
            if (linked && ::android::hardware::bluetooth::V1_0::implementation::BluetoothAddress::
                              get_local_address(address)) {
                session->fd = bluetooth_controller_acquire(address);
                if (session->fd >= 0) {
                    int flags = fcntl(session->fd, F_GETFL);
                    if (flags < 0 || fcntl(session->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                        bluetooth_controller_release();
                        session->fd = -1;
                    }
                }
                if (session->fd >= 0) {
                    session_ = session;
                    result = Status::SUCCESS;
                }
            }
            if (result != Status::SUCCESS && session->death_cookie)
                AIBinder_unlinkToDeath(callback->asBinder().get(), death_.get(), session->death_cookie);
        }
    }
    // AIDL callbacks are synchronous: never call the client while holding the state lock.
    auto status = callback->initializationComplete(result);
    if (result == Status::SUCCESS) {
        if (!status.isOk()) {
            closeSession(session);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_ == session) session->reader = std::thread(receive, session);
        }
    }
    return status;
}

void BluetoothHci::closeSession(const std::shared_ptr<Session>& expected) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_ || (expected && expected != session_)) return;
        session = std::move(session_);
        closing_ = true;
        session->stopping = true;
    }
    session->wake();
    if (session->reader.joinable()) {
        // A synchronous callback can reenter close on the reader's Binder transaction thread.
        // The worker retains its session and checks stopping before accessing the fd again.
        if (session->reader.get_id() == std::this_thread::get_id())
            session->reader.detach();
        else
            session->reader.join();
    }
    if (session->death_cookie)
        AIBinder_unlinkToDeath(session->callback->asBinder().get(), death_.get(), session->death_cookie);
    bluetooth_controller_release();
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = false;
}
ScopedAStatus BluetoothHci::close() {
    closeSession(nullptr);
    return ScopedAStatus::ok();
}

bool BluetoothHci::readExact(const std::shared_ptr<Session>& session, uint8_t* data, size_t size) {
    size_t used = 0;
    while (used < size && !session->stopping && !session->failed) {
        pollfd fds[] = {{session->cancel[0], POLLIN, 0}, {session->fd, POLLIN, 0}};
        int result = poll(fds, 2, -1);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || fds[0].revents || !(fds[1].revents & POLLIN)) return false;
        ssize_t count = recv(session->fd, data + used, size - used, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return false;
        used += count;
    }
    return used == size;
}
void BluetoothHci::hardwareError(const std::shared_ptr<Session>& session) {
    if (session->stopping || session->failed.exchange(true)) return;
    session->wake();
    auto status = session->callback->hciEventReceived({0x10, 1, 0});
    if (!status.isOk()) ALOGW("Cannot notify the Bluetooth client of controller loss");
}
void BluetoothHci::receive(std::shared_ptr<Session> session) {
#if defined(__ANDROID__)
    // Match the AOSP HCI reader priority, including SCO traffic under CPU load.
    sched_param priority{};
    priority.sched_priority = 1;
    if (sched_setscheduler(0, SCHED_FIFO, &priority) < 0)
        ALOGW("Cannot set Bluetooth receive priority: %s", strerror(errno));
#endif
    while (!session->stopping && !session->failed) {
        uint8_t type;
        if (!readExact(session, &type, 1)) break;
        size_t header = type == 2 ? 4 : type == 3 ? 3 : type == 4 ? 2 : 0;
        if (!header) break;
        std::vector<uint8_t> packet(header);
        if (!readExact(session, packet.data(), header)) break;
        size_t payload = type == 2 ? packet[2] | (packet[3] << 8) : packet[header - 1];
        packet.resize(header + payload);
        if (payload && !readExact(session, packet.data() + header, payload)) break;
        if (session->stopping || session->failed) break;
        if (type == 4 && packet[0] == 0x10) break;
        auto status = type == 2   ? session->callback->aclDataReceived(packet)
                      : type == 3 ? session->callback->scoDataReceived(packet)
                                  : session->callback->hciEventReceived(packet);
        if (!status.isOk()) break;
    }
    hardwareError(session);
}
ScopedAStatus BluetoothHci::send(uint8_t type, const std::vector<uint8_t>& packet) {
    size_t header = type == 2 ? 4 : 3;
    if (packet.size() < header) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    size_t payload = type == 2 ? packet[2] | (packet[3] << 8) : packet[2];
    if (packet.size() != header + payload)
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    std::shared_ptr<Session> session;
    {
        // Serialize complete H4 packets and prevent fd release during a write.
        std::lock_guard<std::mutex> lock(mutex_);
        session = session_;
        if (!session || session->failed)
            return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        std::vector<uint8_t> framed;
        framed.reserve(packet.size() + 1);
        framed.push_back(type);
        framed.insert(framed.end(), packet.begin(), packet.end());
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        size_t used = 0;
        while (used < framed.size() && !session->failed) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now())
                                 .count();
            if (remaining <= 0) break;
            pollfd fd{session->fd, POLLOUT, 0};
            int result = poll(&fd, 1, remaining);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
            ssize_t count = ::send(session->fd, framed.data() + used, framed.size() - used,
                                   MSG_DONTWAIT | MSG_NOSIGNAL);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) break;
            used += count;
        }
        if (used == framed.size()) return ScopedAStatus::ok();
    }
    hardwareError(session);
    return ScopedAStatus::fromServiceSpecificError(EIO);
}
ScopedAStatus BluetoothHci::sendHciCommand(const std::vector<uint8_t>& p) { return send(1, p); }
ScopedAStatus BluetoothHci::sendAclData(const std::vector<uint8_t>& p) { return send(2, p); }
ScopedAStatus BluetoothHci::sendScoData(const std::vector<uint8_t>& p) { return send(3, p); }
ScopedAStatus BluetoothHci::sendIsoData(const std::vector<uint8_t>&) {
    // This controller predates LE Isochronous Channels; never put H4 type 5 on its UART.
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}
}  // namespace universal7880

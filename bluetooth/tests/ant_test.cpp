/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "../ant/Ant.h"
#include "ant_controller.h"
#include "bluetooth_address.h"

using android::hardware::hidl_string;
using android::hardware::hidl_vec;
using android::hardware::Return;
using android::hardware::Void;
using android::sp;
using com::dsi::ant::V1_0::IAntCallbacks;

static int controller[2] = {-1, -1};
static int releases = 0;
extern "C" int ant_controller_acquire(const uint8_t[6]) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, controller) == 0);
    int capacity = 1024;
    assert(setsockopt(controller[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
    return controller[0];
}
extern "C" void ant_controller_release() {
    close(controller[0]);
    close(controller[1]);
    controller[0] = controller[1] = -1;
    ++releases;
}
bool android::hardware::bluetooth::V1_0::implementation::BluetoothAddress::get_local_address(
    uint8_t* address) {
    for (int i = 0; i < 6; ++i) address[i] = i + 1;
    return true;
}

class Callbacks : public IAntCallbacks {
   public:
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<uint8_t> message;
    int failures = 0;
    sp<android::hardware::hidl_death_recipient> death;
    uint64_t cookie = 0;
    Return<bool> linkToDeath(const sp<android::hardware::hidl_death_recipient>& recipient,
                            uint64_t value) override {
        death = recipient;
        cookie = value;
        return true;
    }
    Return<bool> unlinkToDeath(const sp<android::hardware::hidl_death_recipient>&) override {
        death.clear();
        return true;
    }
    Return<void> onMessageReceived(const hidl_vec<uint8_t>& data) override {
        std::lock_guard<std::mutex> lock(mutex);
        message.assign(data.begin(), data.end());
        changed.notify_all();
        return Void();
    }
    Return<void> onTransportDown(const hidl_string&) override {
        std::lock_guard<std::mutex> lock(mutex);
        ++failures;
        changed.notify_all();
        return Void();
    }
    template <class Predicate> void wait(Predicate predicate) {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(5), predicate));
    }
};

static void expect(int fd, const std::vector<uint8_t>& wanted) {
    std::vector<uint8_t> actual(wanted.size());
    size_t used = 0;
    while (used < actual.size()) {
        pollfd p{fd, POLLIN, 0};
        assert(poll(&p, 1, 1000) > 0);
        ssize_t count = read(fd, actual.data() + used, actual.size() - used);
        assert(count > 0);
        used += count;
    }
    assert(actual == wanted);
}

int main() {
    sp<universal7880::Ant> ant = new universal7880::Ant();
    sp<Callbacks> callback = new Callbacks();
    ant->setCallbacks(callback);
    assert(ant->enable() == 0);
    hidl_vec<uint8_t> invalid{0};
    assert(ant->sendCommandMessage(invalid) == -EINVAL);
    hidl_vec<uint8_t> command{1, 0x4a, 0};
    assert(ant->sendCommandMessage(command) == 0);
    expect(controller[1], {0x0c, 3, 1, 0x4a, 0});
    const uint8_t flow[] = {0x0e, 3, 1, 0xc9, 0};
    assert(write(controller[1], flow, sizeof(flow)) == sizeof(flow));
    callback->wait([&] { return callback->message == std::vector<uint8_t>({1, 0xc9, 0}); });

    // Saturated TX must notify recovery even though RX is still waiting for input.
    hidl_vec<uint8_t> data;
    data.resize(255);
    data[0] = 253;
    data[1] = 0x4f;
    int status = 0;
    for (int i = 0; i < 4096 && status == 0; ++i) status = ant->sendDataMessage(data);
    assert(status == -EIO);
    callback->wait([&] { return callback->failures == 1; });
    assert(ant->sendCommandMessage(command) == -ENOTCONN);
    assert(ant->disable() == 0 && releases == 1);
    assert(callback->failures == 1);  // RX cancellation must not report the same failure twice.

    // Recovery clears the old failure; reject a malformed incoming ANT message.
    assert(ant->enable() == 0);
    assert(ant->sendCommandMessage(command) == 0);
    expect(controller[1], {0x0c, 3, 1, 0x4a, 0});
    const uint8_t malformed[] = {0x0e, 2, 0, 0xc9};
    assert(write(controller[1], malformed, sizeof(malformed)) == sizeof(malformed));
    callback->wait([&] { return callback->failures == 2; });
    assert(ant->disable() == 0 && releases == 2);

    assert(ant->enable() == 0);
    callback->death->serviceDied(callback->cookie, {});
    assert(releases == 3 && ant->enable() == -ENOTCONN);
    ant->setCallbacks(nullptr);
    assert(ant->disable() == 0 && releases == 3);
    return 0;
}

/* SPDX-License-Identifier: Apache-2.0 */
#include <aidl/android/hardware/bluetooth/BnBluetoothHciCallbacks.h>
#include <assert.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

#include "../aidl/BluetoothHci.h"
#include "bluetooth_address.h"
#include "bluetooth_controller.h"

using aidl::android::hardware::bluetooth::BnBluetoothHciCallbacks;
using aidl::android::hardware::bluetooth::Status;
using ndk::ScopedAStatus;
static int controller[2] = {-1, -1}, releases = 0;
extern "C" int bluetooth_controller_acquire(const uint8_t[6]) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, controller) == 0);
    int size = 1024;
    assert(setsockopt(controller[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
    return controller[0];
}
extern "C" void bluetooth_controller_release() {
    ::close(controller[0]);
    ::close(controller[1]);
    controller[0] = controller[1] = -1;
    ++releases;
}
bool android::hardware::bluetooth::V1_0::implementation::BluetoothAddress::get_local_address(
    uint8_t* address) {
    for (int i = 0; i < 6; ++i) address[i] = i + 1;
    return true;
}
class Callbacks : public BnBluetoothHciCallbacks {
   public:
    std::mutex mutex;
    std::condition_variable changed;
    Status initialized = Status::UNKNOWN;
    std::vector<uint8_t> event, acl, sco;
    std::function<void()> on_init, on_event;
    ScopedAStatus initializationComplete(Status status) override {
        initialized = status;
        if (on_init) on_init();
        return ScopedAStatus::ok();
    }
    ScopedAStatus hciEventReceived(const std::vector<uint8_t>& packet) override {
        if (on_event) on_event();
        std::lock_guard<std::mutex> lock(mutex);
        event = packet;
        changed.notify_all();
        return ScopedAStatus::ok();
    }
    ScopedAStatus aclDataReceived(const std::vector<uint8_t>& p) override {
        std::lock_guard<std::mutex> lock(mutex);
        acl = p;
        changed.notify_all();
        return ScopedAStatus::ok();
    }
    ScopedAStatus scoDataReceived(const std::vector<uint8_t>& p) override {
        std::lock_guard<std::mutex> lock(mutex);
        sco = p;
        changed.notify_all();
        return ScopedAStatus::ok();
    }
    ScopedAStatus isoDataReceived(const std::vector<uint8_t>&) override { abort(); }
    template <class Predicate> void wait(Predicate predicate) {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(5), predicate));
    }
};
static void expect(const std::vector<uint8_t>& wanted) {
    std::vector<uint8_t> actual(wanted.size());
    size_t used = 0;
    while (used < actual.size()) {
        pollfd p{controller[1], POLLIN, 0};
        assert(poll(&p, 1, 1000) > 0);
        ssize_t count = read(controller[1], actual.data() + used, actual.size() - used);
        assert(count > 0);
        used += count;
    }
    assert(actual == wanted);
}
int main() {
    auto hal = ndk::SharedRefBase::make<universal7880::BluetoothHci>();
    auto cb = ndk::SharedRefBase::make<Callbacks>();
    assert(!hal->initialize(nullptr).isOk());
    assert(!hal->sendHciCommand({3, 12, 0}).isOk());
    assert(hal->initialize(cb).isOk() && cb->initialized == Status::SUCCESS);
    auto other = ndk::SharedRefBase::make<Callbacks>();
    assert(hal->initialize(other).isOk() && other->initialized == Status::ALREADY_INITIALIZED);
    assert(hal->sendHciCommand({3, 12, 0}).isOk());
    expect({1, 3, 12, 0});
    assert(!hal->sendHciCommand({3, 12}).isOk());
    assert(!hal->sendAclData({1, 0, 2, 0, 1}).isOk());
    assert(!hal->sendIsoData({1, 0, 0, 0}).isOk());
    assert(hal->sendAclData({1, 0, 1, 0, 0x55}).isOk());
    expect({2, 1, 0, 1, 0, 0x55});
    const uint8_t rx[] = {4, 14, 4, 1, 3, 12, 0, 2, 1, 0, 1, 0, 0x44, 3, 1, 0, 1, 0x33};
    assert(write(controller[1], rx, sizeof(rx)) == sizeof(rx));
    cb->wait([&] { return cb->event.size() == 6 && cb->acl.size() == 5 && cb->sco.size() == 4; });
    assert(hal->close().isOk() && releases == 1);
    assert(hal->close().isOk() && releases == 1);

    // Synchronous AIDL callbacks may reenter close, including on the reader thread.
    cb->on_init = [&] { hal->close(); };
    assert(hal->initialize(cb).isOk() && releases == 2);
    cb->on_init = {};
    cb->event.clear();
    cb->on_event = [&] { hal->close(); };
    assert(hal->initialize(cb).isOk());
    assert(write(controller[1], rx, 7) == 7);
    cb->wait([&] { return !cb->event.empty(); });
    assert(releases == 3);
    cb->on_event = {};
    cb->event.clear();
    assert(hal->initialize(cb).isOk());
    std::vector<uint8_t> acl(65539, 0x55);
    acl[0] = 1; acl[1] = 0; acl[2] = acl[3] = 255;
    assert(!hal->sendAclData(acl).isOk());
    cb->wait([&] { return cb->event == std::vector<uint8_t>({0x10, 1, 0}); });
    assert(!hal->sendHciCommand({3, 12, 0}).isOk());
    assert(hal->close().isOk() && releases == 4);
    return 0;
}

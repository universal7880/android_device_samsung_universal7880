/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "bt_transport"

#include "h4_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace {

struct Packet {
    std::array<uint8_t, 65540> bytes{};
    size_t used = 0, needed = 1, written = 0;
    bool ready = false;

    void reset() {
        used = written = 0;
        needed = 1;
        ready = false;
    }
    bool ant() const { return bytes[0] == 0x0c || bytes[0] == 0x0e; }

    bool advance(bool from_controller, bool ant_client = false) {
        if (used == 1) {
            if (!from_controller && ant() != ant_client) return false;
            switch (bytes[0]) {
                case 1:
                    if (from_controller) return false;
                    needed = 4;
                    break;
                case 2:
                    needed = 5;
                    break;
                case 3:
                    needed = 4;
                    break;
                case 4:
                    if (!from_controller) return false;
                    needed = 3;
                    break;
                case 0x0c:
                case 0x0e:
                    needed = 2;
                    break;
                default:
                    /* GPIO wake is used; these bytes are not an IBS handshake. */
                    if (from_controller && bytes[0] >= 0xfc && bytes[0] <= 0xfe) {
                        reset();
                        return true;
                    }
                    return false;
            }
        } else {
            size_t header = ant() ? 2 : bytes[0] == 2 ? 5 : bytes[0] == 4 ? 3 : 4;
            if (used == header)
                needed = header + (bytes[0] == 2 ? bytes[3] | (bytes[4] << 8) : bytes[header - 1]);
            ready = used == needed;
        }
        return true;
    }
};

bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

struct Channel {
    int sockets[2] = {-1, -1};
    bool enabled = false;
    uint64_t generation = 0;
    Packet tx;
};

class Transport {
   public:
    int uart = -1, uart_flags = -1;
    int cancel[2] = {-1, -1};
    std::array<Channel, 2> channels;
    std::thread worker;
    std::mutex mutex;

    ~Transport() {
        if (worker.joinable()) {
            stopping = true;
            wake(1);
            worker.join();
        }
        if (uart_flags >= 0) fcntl(uart, F_SETFL, uart_flags);
        for (auto& channel : channels)
            for (int fd : channel.sockets)
                if (fd >= 0) close(fd);
        for (int fd : cancel)
            if (fd >= 0) close(fd);
    }

    bool start(int fd, int first) {
        uart = fd;
        uart_flags = fcntl(uart, F_GETFL);
        if (uart_flags < 0 || pipe(cancel) < 0) return false;
        for (auto& channel : channels) {
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel.sockets) < 0) return false;
            for (int owned : channel.sockets)
                if (fcntl(owned, F_SETFD, FD_CLOEXEC) < 0) return false;
            if (!nonblocking(channel.sockets[1])) return false;
        }
        for (int owned : cancel)
            if (fcntl(owned, F_SETFD, FD_CLOEXEC) < 0 || !nonblocking(owned)) return false;
        if (!nonblocking(uart)) return false;
        channels[first].enabled = true;
        worker = std::thread(&Transport::run, this);
        return true;
    }

    int enable(int id, bool on) {
        std::lock_guard<std::mutex> lock(mutex);
        if (dead) return -1;
        auto& channel = channels[id];
        channel.enabled = on;
        ++channel.generation;
        /* Both HALs stop reading before release. Clear the previous session's bytes. */
        uint8_t discard[1024];
        for (int fd : channel.sockets)
            while (recv(fd, discard, sizeof(discard), MSG_DONTWAIT) > 0) {
            }
        if (!channel.tx.written) channel.tx.reset();
        if (rx.ready && static_cast<int>(rx.ant()) == id) rx.reset();
        wake(0);
        return channel.sockets[0];
    }

   private:
    Packet rx;
    bool dead = false, controller_crashed = false;
    std::atomic<bool> stopping{false};
    uint64_t rx_generation = 0;
    std::chrono::steady_clock::time_point forward_deadline;

    void wake(uint8_t value) {
        while (write(cancel[1], &value, 1) < 0 && errno == EINTR) {
        }
    }

    bool receive(int fd, Packet& packet, bool from_controller, bool ant_client = false) {
        ssize_t size = read(fd, packet.bytes.data() + packet.used, packet.needed - packet.used);
        if (size < 0) return errno == EINTR || errno == EAGAIN;
        if (size == 0) return false;
        packet.used += size;
        if (from_controller && packet.used == 1) rx_generation = channels[packet.ant()].generation;
        if (packet.used != packet.needed) return true;
        if (!packet.advance(from_controller, ant_client)) {
            ALOGE("Unexpected H4 packet type 0x%02x", packet.bytes[0]);
            return false;
        }
        if (from_controller && packet.ready && packet.bytes[0] == 4 && packet.bytes[1] == 0xff &&
            packet.bytes[2] >= 2 && packet.bytes[3] == 1) {
            if (packet.bytes[4] == 8) {
                ALOGE("Controller firmware crashed");
                packet.bytes[1] = 0x10;
                packet.bytes[2] = 1;
                packet.bytes[3] = 0;
                packet.used = packet.needed = 4;
                controller_crashed = true;
            } else {
                packet.reset();
            }
        }
        if (from_controller && packet.ready)
            forward_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        if (from_controller && packet.ready &&
            (!channels[packet.ant()].enabled || rx_generation != channels[packet.ant()].generation))
            packet.reset();
        return true;
    }

    bool transmit(int fd, Packet& packet, bool socket) {
        const auto* data = packet.bytes.data() + packet.written;
        size_t remaining = packet.used - packet.written;
        ssize_t size =
            socket ? send(fd, data, remaining, MSG_NOSIGNAL) : write(fd, data, remaining);
        if (size < 0) return errno == EINTR || errno == EAGAIN;
        if (size == 0) return false;
        packet.written += size;
        if (packet.written == packet.used) packet.reset();
        return true;
    }

    void run() {
        bool failed = false, error_queued = false;
        int writing = -1, next = 0;
        auto fail = [&]() {
            failed = true;
            for (auto& channel : channels) channel.tx.reset();
            if (!rx.ready || !rx.written) rx.reset();
        };
        for (;;) {
            pollfd fds[4];
            int timeout = -1;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (failed && !rx.ready) {
                    if (error_queued || !channels[0].enabled) break;
                    rx.bytes[0] = 4;
                    rx.bytes[1] = 0x10;
                    rx.bytes[2] = 1;
                    rx.bytes[3] = 0;
                    rx.used = rx.needed = 4;
                    rx.ready = true;
                    error_queued = true;
                    forward_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                }
                if (rx.ready) {
                    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         forward_deadline - std::chrono::steady_clock::now())
                                         .count();
                    if (remaining <= 0) break;
                    timeout = static_cast<int>(remaining);
                }
                bool pending = channels[0].tx.ready || channels[1].tx.ready;
                fds[0] = {cancel[0], POLLIN, 0};
                fds[1] = {failed ? -1 : uart,
                          static_cast<short>((rx.ready ? 0 : POLLIN) | (pending ? POLLOUT : 0)), 0};
                for (int id = 0; id < 2; ++id)
                    fds[id + 2] = {
                        channels[id].enabled ? channels[id].sockets[1] : -1,
                        static_cast<short>(
                            (failed || channels[id].tx.ready ? 0 : POLLIN) |
                            (rx.ready && static_cast<int>(rx.ant()) == id ? POLLOUT : 0)),
                        0};
            }
            int result = poll(fds, 4, timeout);
            if (result == 0) {
                ALOGE("HAL receive queue stopped draining");
                break;
            }
            if (result < 0) {
                if (errno == EINTR) continue;
                break;
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) break;
            if (fds[0].revents) {
                uint8_t values[64];
                ssize_t count;
                bool stop = false;
                while ((count = read(cancel[0], values, sizeof(values))) > 0)
                    for (ssize_t i = 0; i < count; ++i) stop |= values[i] == 1;
                if (stop) break;
                continue;
            }
            if (fds[1].revents & (POLLERR | POLLNVAL)) fail();
            if (!failed && (fds[1].revents & POLLIN) && !receive(uart, rx, true)) fail();
            if (controller_crashed) {
                failed = error_queued = true;
                for (auto& channel : channels) channel.tx.reset();
            }
            bool client_failed = false;
            for (int id = 0; id < 2; ++id) {
                if (!channels[id].enabled) continue;
                if (fds[id + 2].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    client_failed = true;
                    break;
                }
                if (!failed && (fds[id + 2].revents & POLLIN) &&
                    !receive(channels[id].sockets[1], channels[id].tx, false, id == 1)) {
                    client_failed = true;
                    break;
                }
            }
            if (client_failed) fail();
            if (!failed && (fds[1].revents & POLLOUT)) {
                if (writing >= 0 && !channels[writing].tx.ready) writing = -1;
                if (writing < 0) {
                    if (channels[next].tx.ready)
                        writing = next;
                    else if (channels[1 - next].tx.ready)
                        writing = 1 - next;
                }
                if (writing >= 0) {
                    if (!transmit(uart, channels[writing].tx, false)) fail();
                    if (!channels[writing].tx.ready) {
                        next = 1 - writing;
                        writing = -1;
                    }
                }
            }
            int destination = rx.ant();
            if (rx.ready && channels[destination].enabled &&
                (fds[destination + 2].revents & POLLOUT) &&
                !transmit(channels[destination].sockets[1], rx, true))
                break;
            if (!failed && (fds[1].revents & POLLHUP) && !(fds[1].revents & POLLIN)) fail();
        }
        std::lock_guard<std::mutex> lock(mutex);
        dead = true;
        for (auto& channel : channels) shutdown(channel.sockets[1], SHUT_RDWR);
    }
};

std::unique_ptr<Transport> transport;
int start(int uart, int channel) {
    if (transport) return -1;
    auto instance = std::make_unique<Transport>();
    if (!instance->start(uart, channel)) return -1;
    int fd = instance->channels[channel].sockets[0];
    transport = std::move(instance);
    return fd;
}
}  // namespace

extern "C" int h4_transport_start(int fd) { return start(fd, 0); }
extern "C" int h4_transport_start_ant(int fd) { return start(fd, 1); }
extern "C" int h4_transport_enable(int channel, int on) {
    if (!transport || channel < 0 || channel > 1) return -1;
    return transport->enable(channel, on != 0);
}
extern "C" void h4_transport_stop() { transport.reset(); }

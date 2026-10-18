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
#include <cstdint>
#include <memory>
#include <thread>

namespace {

/* One complete ACL packet in each direction bounds buffering and backpressure. */
struct Packet {
    std::array<uint8_t, 65540> bytes{};
    size_t used = 0;
    size_t needed = 1;
    size_t written = 0;
    bool ready = false;

    void reset() {
        used = written = 0;
        needed = 1;
        ready = false;
    }

    bool advance(bool from_controller) {
        if (used == 1) {
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
                default:
                    /* Samsung uses GPIO wake; these are discarded, not an IBS handshake. */
                    if (from_controller && bytes[0] >= 0xfc && bytes[0] <= 0xfe) {
                        reset();
                        return true;
                    }
                    return false;
            }
        } else {
            size_t header = bytes[0] == 2 ? 5 : bytes[0] == 4 ? 3 : 4;
            if (used == header) {
                needed = header + (bytes[0] == 2 ? bytes[3] | (bytes[4] << 8) : bytes[header - 1]);
            }
            ready = used == needed;
        }
        return true;
    }
};

bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

class Transport {
   public:
    int uart = -1;
    int sockets[2] = {-1, -1};
    int cancel[2] = {-1, -1};
    int uart_flags = -1;
    std::thread worker;

    ~Transport() {
        if (worker.joinable()) {
            const uint8_t stop = 1;
            while (write(cancel[1], &stop, 1) < 0 && errno == EINTR) {
            }
            worker.join();
        }
        if (uart_flags >= 0) fcntl(uart, F_SETFL, uart_flags);
        for (int fd : sockets)
            if (fd >= 0) close(fd);
        for (int fd : cancel)
            if (fd >= 0) close(fd);
    }

    bool start(int fd) {
        uart = fd;
        uart_flags = fcntl(uart, F_GETFL);
        if (uart_flags < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0 || pipe(cancel) < 0)
            return false;
        for (int owned : {sockets[0], sockets[1], cancel[0], cancel[1]}) {
            if (fcntl(owned, F_SETFD, FD_CLOEXEC) < 0) return false;
        }
        if (!nonblocking(uart) || !nonblocking(sockets[1]) || !nonblocking(cancel[0]) ||
            !nonblocking(cancel[1]))
            return false;
        worker = std::thread(&Transport::run, this);
        return true;
    }

   private:
    bool controller_crashed = false;

    bool receive(int fd, Packet& packet, bool from_controller) {
        ssize_t n = read(fd, packet.bytes.data() + packet.used, packet.needed - packet.used);
        if (n < 0) return errno == EINTR || errno == EAGAIN;
        if (n == 0) return false;
        packet.used += n;
        if (packet.used != packet.needed) return true;
        if (!packet.advance(from_controller)) {
            ALOGE("Unexpected H4 packet type 0x%02x", packet.bytes[0]);
            return false;
        }
        if (from_controller && packet.ready && packet.bytes[0] == 4 && packet.bytes[1] == 0xff &&
            packet.bytes[2] >= 2 && packet.bytes[3] == 1) {
            if (packet.bytes[4] == 8) {
                /* Let the Bluetooth stack recover instead of forwarding a firmware dump. */
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
        return true;
    }

    bool transmit(int fd, Packet& packet, bool to_socket) {
        const auto* data = packet.bytes.data() + packet.written;
        size_t remaining = packet.used - packet.written;
        ssize_t n =
            to_socket ? send(fd, data, remaining, MSG_NOSIGNAL) : write(fd, data, remaining);
        if (n < 0) return errno == EINTR || errno == EAGAIN;
        if (n == 0) return false;
        packet.written += n;
        if (packet.written == packet.used) packet.reset();
        return true;
    }

    void run() {
        Packet rx, tx;
        bool failed = false;
        bool error_queued = false;
        auto controller_error = [&]() {
            failed = true;
            tx.reset();
            /* Finish a partly forwarded packet before reporting the failure. */
            if (!rx.ready || !rx.written) rx.reset();
        };
        for (;;) {
            if (failed && !rx.ready) {
                if (error_queued) break;
                rx.bytes[0] = 4;
                rx.bytes[1] = 0x10;
                rx.bytes[2] = 1;
                rx.bytes[3] = 0;
                rx.used = rx.needed = 4;
                rx.ready = true;
                error_queued = true;
            }
            pollfd fds[] = {
                {cancel[0], POLLIN, 0},
                {failed ? -1 : uart,
                 static_cast<short>((rx.ready ? 0 : POLLIN) | (tx.ready ? POLLOUT : 0)), 0},
                {sockets[1],
                 static_cast<short>((failed || tx.ready ? 0 : POLLIN) | (rx.ready ? POLLOUT : 0)),
                 0},
            };
            /* No periodic polling, timer or userspace wakelock while idle. */
            int ret = poll(fds, 3, -1);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (fds[0].revents) break;
            if (fds[2].revents & (POLLERR | POLLNVAL | POLLHUP)) break;
            if (fds[1].revents & (POLLERR | POLLNVAL)) controller_error();
            if (!failed && (fds[1].revents & POLLIN) && !receive(uart, rx, true))
                controller_error();
            if (controller_crashed) {
                failed = error_queued = true;
                tx.reset();
            }
            if (!failed && (fds[2].revents & POLLIN) && !receive(sockets[1], tx, false)) break;
            if (!failed && (fds[1].revents & POLLOUT) && !transmit(uart, tx, false))
                controller_error();
            if (rx.ready && (fds[2].revents & POLLOUT) && !transmit(sockets[1], rx, true)) break;
            if (!failed && (fds[1].revents & POLLHUP) && !(fds[1].revents & POLLIN))
                controller_error();
        }
        shutdown(sockets[1], SHUT_RDWR);
    }
};

std::unique_ptr<Transport> transport;

}  // namespace

extern "C" int h4_transport_start(int uart_fd) {
    if (transport) return -1;
    auto instance = std::make_unique<Transport>();
    if (!instance->start(uart_fd)) return -1;
    int fd = instance->sockets[0];
    transport = std::move(instance);
    return fd;
}

extern "C" void h4_transport_stop() { transport.reset(); }

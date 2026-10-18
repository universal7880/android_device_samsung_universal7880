/* SPDX-License-Identifier: Apache-2.0 */
#include "h4_transport.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <vector>

static void write_all(int fd, const std::vector<unsigned char>& bytes) {
    size_t used = 0;
    while (used < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + used, bytes.size() - used);
        assert(n > 0);
        used += n;
    }
}

static void expect(int fd, const std::vector<unsigned char>& expected) {
    std::vector<unsigned char> actual(expected.size());
    size_t used = 0;
    while (used < actual.size()) {
        pollfd pfd{fd, POLLIN, 0};
        assert(poll(&pfd, 1, 1000) > 0);
        ssize_t n = read(fd, actual.data() + used, actual.size() - used);
        assert(n > 0);
        used += n;
    }
    assert(actual == expected);
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    for (int cycle = 0; cycle < 20; ++cycle) {
        int uart[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, uart) == 0);
        int flags = fcntl(uart[0], F_GETFL);
        int host = h4_transport_start(uart[0]);
        assert(host >= 0);
        assert(h4_transport_start(uart[0]) < 0);

        /* Fragmented header/payload, zero-length command, consecutive packets. */
        write_all(host, {1});
        write_all(host, {3, 12});
        write_all(host, {0, 1, 1, 16, 1, 42});
        expect(uart[1], {1, 3, 12, 0, 1, 1, 16, 1, 42});

        /* Strip controller diagnostics and GPIO-mode sleep bytes, retain events. */
        write_all(uart[1], {0xfc, 0xfd, 0xfe, 4, 0xff, 3, 1, 7, 0});
        write_all(uart[1], {4, 0x0e, 4, 1, 3, 12, 0});
        expect(host, {4, 0x0e, 4, 1, 3, 12, 0});
        write_all(uart[1], {3, 1, 0, 2, 17, 18});
        expect(host, {3, 1, 0, 2, 17, 18});

        /* The maximum declared ACL length must survive without truncation. */
        std::vector<unsigned char> acl(65540, 0x5a);
        acl[0] = 2;
        acl[1] = 1;
        acl[2] = 0;
        acl[3] = 0xff;
        acl[4] = 0xff;
        write_all(host, acl);
        expect(uart[1], acl);
        write_all(uart[1], acl);
        expect(host, acl);

        /* Cancellation must not wait for an unfinished frame or a slow reader. */
        write_all(host, {2, 1});
        auto start = std::chrono::steady_clock::now();
        h4_transport_stop();
        assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
        assert((fcntl(uart[0], F_GETFL) & O_NONBLOCK) == (flags & O_NONBLOCK));
        h4_transport_stop();
        close(uart[0]);
        close(uart[1]);
    }

    for (int failure = 0; failure < 3; ++failure) {
        int uart[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, uart) == 0);
        int host = h4_transport_start(uart[0]);
        assert(host >= 0);
        if (failure == 0) {
            close(uart[1]);
        } else if (failure == 1) {
            write_all(uart[1], {4, 0xff, 2, 1, 8});
        } else {
            write_all(uart[1], {0x77});
        }
        expect(host, {4, 0x10, 1, 0});
        pollfd pfd{host, POLLIN, 0};
        assert(poll(&pfd, 1, 1000) > 0);
        unsigned char byte;
        assert(read(host, &byte, 1) == 0);
        h4_transport_stop();
        close(uart[0]);
        if (failure != 0) close(uart[1]);
    }

    /* A full UART output queue must not keep shutdown or its owner alive. */
    int uart[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, uart) == 0);
    int host = h4_transport_start(uart[0]);
    assert(host >= 0);
    assert(fcntl(host, F_SETFL, O_NONBLOCK) == 0);
    std::vector<unsigned char> traffic(65540, 0x5a);
    traffic[0] = 2;
    traffic[1] = 1;
    traffic[2] = 0;
    traffic[3] = traffic[4] = 0xff;
    size_t offset = 0;
    for (int attempt = 0; attempt < 10000; ++attempt) {
        ssize_t n = write(host, traffic.data() + offset, traffic.size() - offset);
        if (n < 0) break;
        assert(n > 0);
        offset = (offset + n) % traffic.size();
    }
    auto start = std::chrono::steady_clock::now();
    h4_transport_stop();
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    close(uart[0]);
    close(uart[1]);
    return 0;
}

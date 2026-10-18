/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>
#include <vector>

#include "h4_transport.h"

static void writeAll(int fd, const std::vector<unsigned char>& data) {
    size_t used = 0;
    while (used < data.size()) {
        ssize_t n = write(fd, data.data() + used, data.size() - used);
        assert(n > 0);
        used += n;
    }
}
static void expect(int fd, const std::vector<unsigned char>& wanted) {
    std::vector<unsigned char> data(wanted.size());
    size_t used = 0;
    while (used < data.size()) {
        pollfd p{fd, POLLIN, 0};
        assert(poll(&p, 1, 1000) > 0);
        ssize_t n = read(fd, data.data() + used, data.size() - used);
        assert(n > 0);
        used += n;
    }
    assert(data == wanted);
}
static void expectEof(int fd) {
    pollfd p{fd, POLLIN, 0};
    assert(poll(&p, 1, 1000) > 0);
    unsigned char byte;
    assert(read(fd, &byte, 1) == 0);
}
int main() {
    signal(SIGPIPE, SIG_IGN);
    const std::vector<unsigned char> command{1, 3, 12, 0};
    const std::vector<unsigned char> event{4, 14, 4, 1, 3, 12, 0};
    const std::vector<unsigned char> antControl{0x0c, 3, 1, 0x4a, 0};
    const std::vector<unsigned char> antData{0x0e, 3, 1, 0xc9, 0};
    for (int first = 0; first < 2; ++first) {
        int uart[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, uart) == 0);
        int initial = first ? h4_transport_start_ant(uart[0]) : h4_transport_start(uart[0]);
        assert(initial >= 0);
        int bt = first ? h4_transport_enable(0, 1) : initial;
        int ant = first ? initial : h4_transport_enable(1, 1);
        assert(bt >= 0 && ant >= 0);
        std::thread bluetooth([&] {
            for (int i = 0; i < 100; ++i) writeAll(bt, command);
        });
        std::thread antWriter([&] {
            for (int i = 0; i < 100; ++i) writeAll(ant, antControl);
        });
        int btCount = 0, antCount = 0;
        for (int i = 0; i < 200; ++i) {
            pollfd p{uart[1], POLLIN, 0};
            assert(poll(&p, 1, 1000) > 0);
            unsigned char type;
            assert(read(uart[1], &type, 1) == 1);
            if (type == 1) {
                expect(uart[1], {3, 12, 0});
                ++btCount;
            } else {
                assert(type == 0x0c);
                expect(uart[1], {3, 1, 0x4a, 0});
                ++antCount;
            }
        }
        bluetooth.join();
        antWriter.join();
        assert(btCount == 100 && antCount == 100);
        writeAll(uart[1], antData);
        writeAll(uart[1], event);
        expect(ant, antData);
        expect(bt, event);

        /* A disabled protocol must not block the other protocol's incoming events. */
        assert(h4_transport_enable(1, 0) >= 0);
        writeAll(uart[1], antData);
        writeAll(uart[1], event);
        expect(bt, event);
        assert(h4_transport_enable(1, 1) == ant);
        writeAll(uart[1], antControl);
        expect(ant, antControl);
        assert(h4_transport_enable(0, 0) >= 0);
        writeAll(uart[1], event);
        writeAll(uart[1], antData);
        expect(ant, antData);
        assert(h4_transport_enable(0, 1) == bt);

        /* Controller failure reaches both clients, in each client's protocol. */
        close(uart[1]);
        expect(bt, {4, 0x10, 1, 0});
        expectEof(bt);
        expectEof(ant);
        h4_transport_stop();
        close(uart[0]);
    }

    /* A UART write already in progress must finish before switching protocols. */
    int uart[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, uart) == 0);
    int capacity = 1024;
    assert(setsockopt(uart[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
    int bt = h4_transport_start(uart[0]);
    int ant = h4_transport_enable(1, 1);
    std::vector<unsigned char> acl(65540, 0x55);
    acl[0] = 2;
    acl[1] = 1;
    acl[2] = 0;
    acl[3] = acl[4] = 255;
    writeAll(bt, acl);
    pollfd p{uart[1], POLLIN, 0};
    assert(poll(&p, 1, 1000) > 0);
    assert(h4_transport_enable(0, 0) >= 0);
    writeAll(ant, antControl);
    expect(uart[1], acl);
    expect(uart[1], antControl);
    h4_transport_stop();
    close(uart[0]);
    close(uart[1]);
    return 0;
}

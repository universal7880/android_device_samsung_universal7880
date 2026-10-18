/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "bt_vendor"

#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "ant_controller.h"
#include "bluetooth_controller.h"
#include "bt_vendor_qcom.h"
#include "h4_transport.h"
#include "hci_uart.h"

extern int rome_soc_init(int fd, char *bdaddr);
extern void enable_controller_log(int fd, unsigned char wait_for_evt);

static struct bt_qcom_struct context;
struct bt_qcom_struct *q = &context;
static const bt_vendor_callbacks_t *callbacks;
static bool powered;
static bool configured;
static bool transport_running;
static bool bt_powered;
static bool ant_enabled;
static pthread_mutex_t controller_lock = PTHREAD_MUTEX_INITIALIZER;

static int power_control(bool on) {
    char path[96], type[16];
    int state_fd = -1;

    for (int id = 0; id < 32; ++id) {
        snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/type", id);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t size = read(fd, type, sizeof(type));
        close(fd);
        if (size < 9 || memcmp(type, "bluetooth", 9)) continue;
        snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%d/state", id);
        state_fd = open(path, O_WRONLY | O_CLOEXEC);
        break;
    }
    if (state_fd < 0) {
        ALOGE("Cannot open Bluetooth rfkill state: %s", strerror(errno));
        return -1;
    }
    char value = on ? '1' : '0';
    ssize_t size;
    do {
        size = write(state_fd, &value, 1);
    } while (size < 0 && errno == EINTR);
    close(state_fd);
    if (size != 1) return -1;
    powered = on;
    return 0;
}

static void set_address(const uint8_t *address) {
    memset(&context, 0, sizeof(context));
    for (size_t i = 0; i < sizeof(q->bdaddr); ++i)
        q->bdaddr[i] = address[sizeof(q->bdaddr) - 1 - i];
}

static void stop_if_unused(void) {
    if (bt_powered || ant_enabled) return;
    h4_transport_stop();
    userial_vendor_close();
    transport_running = false;
    if (powered) power_control(false);
}

static void close_bluetooth(void) {
    if (transport_running) h4_transport_enable(0, false);
    configured = false;
}

static int init(const bt_vendor_callbacks_t *cb, unsigned char *address) {
    if (!cb || cb->size < sizeof(*cb) || !address) return -1;
    pthread_mutex_lock(&controller_lock);
    int result = -1;
    if (!callbacks && !bt_powered) {
        if (!powered) {
            set_address(address);
            userial_vendor_init();
            /* Reset stale hardware only when no other protocol owns it. */
            if (power_control(false)) goto done;
        }
        callbacks = cb;
        result = 0;
    }
done:
    pthread_mutex_unlock(&controller_lock);
    return result;
}

static int start_transport(bool ant) {
    if (transport_running) return h4_transport_enable(ant ? 1 : 0, true);
    tUSERIAL_CFG config = {
        USERIAL_DATABITS_8 | USERIAL_PARITY_NONE | USERIAL_STOPBITS_1,
        USERIAL_BAUD_115200,
    };
    int uart = userial_vendor_open(&config);
    if (uart < 0 || rome_soc_init(uart, (char *)q->bdaddr) < 0) goto fail;
    enable_controller_log(uart, 0);
    int fd = ant ? h4_transport_start_ant(uart) : h4_transport_start(uart);
    if (fd < 0) goto fail;
    transport_running = true;
    return fd;
fail:
    h4_transport_stop();
    userial_vendor_close();
    power_control(false);
    return -1;
}

static int open_transport(int *fds) {
    pthread_mutex_lock(&controller_lock);
    int result = -1;
    if (powered && bt_powered && !configured && fds) {
        int fd = start_transport(false);
        if (fd >= 0) {
            fds[0] = fd;
            configured = true;
            result = 1;
        }
    }
    pthread_mutex_unlock(&controller_lock);
    return result;
}

__attribute__((visibility("default"))) int ant_controller_acquire(const uint8_t address[6]) {
    if (!address) return -1;
    pthread_mutex_lock(&controller_lock);
    int fd = -1;
    if (ant_enabled) goto done;
    if (!powered) {
        set_address(address);
        userial_vendor_init();
        if (power_control(false) || power_control(true)) goto done;
    }
    fd = start_transport(true);
    if (fd >= 0)
        ant_enabled = true;
    else
        stop_if_unused();
done:
    pthread_mutex_unlock(&controller_lock);
    return fd;
}

__attribute__((visibility("default"))) int bluetooth_controller_acquire(const uint8_t address[6]) {
    if (!address) return -1;
    pthread_mutex_lock(&controller_lock);
    int fd = -1;
    if (bt_powered || configured || callbacks) goto done;
    if (!powered) {
        set_address(address);
        userial_vendor_init();
        if (power_control(false) || power_control(true)) goto done;
    }
    fd = start_transport(false);
    if (fd >= 0)
        bt_powered = configured = true;
    else
        stop_if_unused();
done:
    pthread_mutex_unlock(&controller_lock);
    return fd;
}

__attribute__((visibility("default"))) void bluetooth_controller_release(void) {
    pthread_mutex_lock(&controller_lock);
    close_bluetooth();
    bt_powered = false;
    stop_if_unused();
    pthread_mutex_unlock(&controller_lock);
}

__attribute__((visibility("default"))) void ant_controller_release(void) {
    pthread_mutex_lock(&controller_lock);
    if (ant_enabled && transport_running) h4_transport_enable(1, false);
    ant_enabled = false;
    stop_if_unused();
    pthread_mutex_unlock(&controller_lock);
}

static int op(bt_vendor_opcode_t opcode, void *param) {
    if (!callbacks) return -1;
    switch (opcode) {
        case BT_VND_OP_POWER_CTRL:
            if (!param) return -1;
            pthread_mutex_lock(&controller_lock);
            int power_result = 0;
            if (*(int *)param == BT_VND_PWR_ON) {
                if (!powered) power_result = power_control(true);
                bt_powered = power_result == 0;
            } else if (*(int *)param == BT_VND_PWR_OFF) {
                close_bluetooth();
                bt_powered = false;
                stop_if_unused();
            } else {
                power_result = -1;
            }
            pthread_mutex_unlock(&controller_lock);
            return power_result;
        case BT_VND_OP_USERIAL_OPEN:
            return open_transport(param);
        case BT_VND_OP_USERIAL_CLOSE:
            pthread_mutex_lock(&controller_lock);
            close_bluetooth();
            pthread_mutex_unlock(&controller_lock);
            return 0;
        case BT_VND_OP_FW_CFG:
            callbacks->fwcfg_cb(configured ? BT_VND_OP_RESULT_SUCCESS : BT_VND_OP_RESULT_FAIL);
            return 0;
        case BT_VND_OP_SCO_CFG:
            callbacks->scocfg_cb(BT_VND_OP_RESULT_SUCCESS);
            return 0;
        case BT_VND_OP_GET_LPM_IDLE_TIMEOUT:
            if (!param) return -1;
            *(uint32_t *)param = 1000;
            return 0;
        case BT_VND_OP_LPM_SET_MODE:
            /* bluetooth-power.c owns GPIO wake and idle timers for this UART. */
            callbacks->lpm_cb(BT_VND_OP_RESULT_SUCCESS);
            return 0;
        case BT_VND_OP_LPM_WAKE_SET_STATE:
            return 0;
        case BT_VND_OP_SET_AUDIO_STATE:
            callbacks->audio_state_cb(BT_VND_OP_RESULT_SUCCESS);
            return 0;
        case BT_VND_OP_EPILOG:
            callbacks->epilog_cb(BT_VND_OP_RESULT_SUCCESS);
            return 0;
        default:
            return -1;
    }
}

static void cleanup(void) {
    pthread_mutex_lock(&controller_lock);
    close_bluetooth();
    bt_powered = false;
    callbacks = NULL;
    stop_if_unused();
    pthread_mutex_unlock(&controller_lock);
}

__attribute__((visibility("default")))
const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
    .size = sizeof(bt_vendor_interface_t),
    .init = init,
    .op = op,
    .cleanup = cleanup,
};

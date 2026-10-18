/* SPDX-License-Identifier: Apache-2.0 */
#define LOG_TAG "bt_vendor"

#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

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

static void close_transport(void) {
    h4_transport_stop();
    userial_vendor_close();
    configured = false;
}

static int init(const bt_vendor_callbacks_t *cb, unsigned char *address) {
    if (!cb || cb->size < sizeof(*cb) || !address || callbacks) return -1;
    memset(&context, 0, sizeof(context));
    /* HIDL supplies display order; Qualcomm NVM tag 2 uses little-endian order. */
    for (size_t i = 0; i < sizeof(q->bdaddr); ++i)
        q->bdaddr[i] = address[sizeof(q->bdaddr) - 1 - i];
    callbacks = cb;
    userial_vendor_init();
    /* Recover from a previous HAL crash without trusting persistent init properties. */
    if (power_control(false)) {
        callbacks = NULL;
        return -1;
    }
    return 0;
}

static int open_transport(int *fds) {
    tUSERIAL_CFG config = {
        USERIAL_DATABITS_8 | USERIAL_PARITY_NONE | USERIAL_STOPBITS_1,
        USERIAL_BAUD_115200,
    };
    if (!powered || configured || !fds) return -1;
    int uart = userial_vendor_open(&config);
    if (uart < 0 || rome_soc_init(uart, (char *)q->bdaddr) < 0) goto fail;
    enable_controller_log(uart, 0);
    int fd = h4_transport_start(uart);
    if (fd < 0) goto fail;
    fds[0] = fd;
    configured = true;
    return 1;

fail:
    close_transport();
    power_control(false);
    return -1;
}

static int op(bt_vendor_opcode_t opcode, void *param) {
    if (!callbacks) return -1;
    switch (opcode) {
        case BT_VND_OP_POWER_CTRL:
            if (!param) return -1;
            if (*(int *)param == BT_VND_PWR_OFF) close_transport();
            return power_control(*(int *)param == BT_VND_PWR_ON);
        case BT_VND_OP_USERIAL_OPEN:
            return open_transport(param);
        case BT_VND_OP_USERIAL_CLOSE:
            close_transport();
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
    close_transport();
    if (powered) power_control(false);
    callbacks = NULL;
}

__attribute__((visibility("default")))
const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
    .size = sizeof(bt_vendor_interface_t),
    .init = init,
    .op = op,
    .cleanup = cleanup,
};

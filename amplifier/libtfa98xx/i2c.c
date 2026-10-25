/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 The LineageOS Project */
#include "i2c.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "NXP_I2C.h"

/* NXP's service is serialized by the controller's mutex. */
static int bus_fd = -1;

int tfa_i2c_open(void) {
    bus_fd = TEMP_FAILURE_RETRY(open("/dev/i2c-20", O_RDWR | O_CLOEXEC));
    return bus_fd < 0 ? -errno : 0;
}

void tfa_i2c_close(void) {
    if (bus_fd >= 0) close(bus_fd);
    bus_fd = -1;
}

int NXP_I2C_BufferSize(void) {
    return NXP_I2C_MAX_SIZE;
}

NXP_I2C_Error_t NXP_I2C_WriteRead(unsigned char slave, int write_size,
                                const unsigned char *write_data, int read_size,
                                unsigned char *read_data) {
    if (bus_fd < 0) return NXP_I2C_NoInit;
    /* The NXP API uses an 8-bit address; Linux uses the 7-bit slave address. */
    if (slave != 0x68 || write_size <= 0 || write_size > NXP_I2C_MAX_SIZE ||
        read_size < 0 || read_size > NXP_I2C_MAX_SIZE || !write_data ||
        (read_size && !read_data)) {
        return NXP_I2C_UnsupportedValue;
    }

    struct i2c_msg messages[2] = {
        {.addr = slave >> 1, .len = write_size, .buf = (unsigned char *)write_data},
        {.addr = slave >> 1, .flags = I2C_M_RD, .len = read_size, .buf = read_data},
    };
    struct i2c_rdwr_ioctl_data transfer = {
        .msgs = messages,
        .nmsgs = read_size ? 2 : 1,
    };
    /* A short transfer is a failure. Never replay a partially delivered DSP command. */
    int result = TEMP_FAILURE_RETRY(ioctl(bus_fd, I2C_RDWR, &transfer));
    if (result == (int)transfer.nmsgs) return NXP_I2C_Ok;
    if (result >= 0) return NXP_I2C_NoAck;
    switch (errno) {
        case ETIMEDOUT: return NXP_I2C_TimeOut;
        case EAGAIN: return NXP_I2C_ArbLost;
        default: return NXP_I2C_NoAck;
    }
}

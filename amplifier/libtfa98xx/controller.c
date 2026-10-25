/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 The LineageOS Project */
#define LOG_TAG "tfa9896"
#include "tfa9896.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <log/log.h>

#include "config.h"
#include "tfa_service.h"
#include "tfa.h"
#include "tfa_container.h"
#include "container.h"
#include "i2c.h"

struct tfa9896 {
    void *container;
    bool probed;
    bool enabled;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct tfa9896 *owner;

static enum Tfa98xx_Error calibrate_cold_device(void) {
    enum Tfa98xx_Error error = tfa98xx_open(0);
    if (error != Tfa98xx_Error_Ok) return error;
    int cold = tfa_is_cold(0);
    if (cold <= 0) {
        tfa98xx_close(0);
        return cold < 0 ? Tfa98xx_Error_Other : Tfa98xx_Error_Ok;
    }

    /* Match the vendor cold-start path without clearing existing MTP calibration. */
    error = tfa98xx_set_mute(0, Tfa98xx_Mute_Digital);
    if (error) goto done;
    error = tfa98xx_powerdown(0, 0);
    if (error) goto done;
    int stable = 0;
    for (int i = 0; i < 10; ++i) {
        error = tfa98xx_dsp_system_stable(0, &stable);
        if (error || stable) break;
        usleep(10000);
    }
    if (error) goto done;
    if (!stable) {
        error = Tfa98xx_Error_NoClock;
        goto done;
    }
    error = tfa98xx_powerdown(0, 1);
    if (error) goto done;
    error = tfaRunStartup(0, 0);
    if (error) goto done;
    error = tfaRunColdboot(0, 1);
    if (error) goto done;
    /* Calibration-once; set_mtp avoids a write when the bit already matches. */
    error = tfa98xx_set_mtp(0, 1, 1);
    if (error) goto done;
    error = tfaRunStartDSP(0);
    if (error) goto done;
    error = tfa98xx_set_mute(0, Tfa98xx_Mute_Digital);
    if (error) goto done;
    error = tfaContWriteFiles(0);
    if (error) goto done;
    error = tfaContWriteFilesProf(0, 0, 0);
    if (error) goto done;
    tfa_set_swprof(0, 0);
    error = tfaRunSpeakerCalibration(0, 0);
    if (error) goto done;
    int impedance = tfa_get_calibration_info(0, 0);
    if (impedance <= 0 || impedance > 50000) {
        ALOGE("Invalid speaker calibration: %d mOhm", impedance);
        error = Tfa98xx_Error_Other;
        goto done;
    }
    ALOGI("Speaker calibrated: %d mOhm", impedance);
done:
    tfa98xx_close(0);
    return error;
}

static int read_container(struct tfa9896 *device) {
    int fd = TEMP_FAILURE_RETRY(open("/vendor/etc/Tfa9896.cnt", O_RDONLY | O_CLOEXEC));
    if (fd < 0) return -errno;
    struct stat st;
    int result = -EINVAL;
    if (fstat(fd, &st) || st.st_size < 54 || st.st_size > TFA_MAX_CNT_LENGTH) goto done;
    device->container = malloc(st.st_size);
    if (!device->container) {
        result = -ENOMEM;
        goto done;
    }
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t count = TEMP_FAILURE_RETRY(read(fd, (char *)device->container + used,
                                               st.st_size - used));
        if (count <= 0) goto done;
        used += count;
    }
    if (tfa_container_validate(device->container, used)) goto done;
    if (tfa_load_cnt(device->container, used) != tfa_error_ok) goto done;
    result = 0;
done:
    close(fd);
    return result;
}

struct tfa9896 *tfa9896_open(void) {
    pthread_mutex_lock(&lock);
    if (owner) {
        errno = EBUSY;
        goto fail;
    }
    struct tfa9896 *device = calloc(1, sizeof(*device));
    if (!device) goto fail;
    int result = read_container(device);
    if (!result) result = tfa_i2c_open();
    if (result) {
        ALOGE("Cannot initialize amplifier: %d", result);
        tfa_deinit();
        free(device->container);
        free(device);
        errno = -result;
        goto fail;
    }
    owner = device;
    pthread_mutex_unlock(&lock);
    return device;
fail:
    pthread_mutex_unlock(&lock);
    return NULL;
}

int tfa9896_enable(struct tfa9896 *device, bool enabled) {
    pthread_mutex_lock(&lock);
    int result = -EINVAL;
    if (!device || device != owner) goto done;
    if (!enabled && !device->probed) {
        result = 0;
        goto done;
    }
    if (!enabled) {
        result = tfa_stop() == tfa_error_ok ? 0 : -EIO;
        if (!result) device->enabled = false;
        goto done;
    }
    if (device->enabled) {
        result = 0;
        goto done;
    }

    /* The vendor controller waits for the supplied I2S clock to stabilize. */
    usleep(80000);
    if (!device->probed) {
        int handle = -1;
        if (tfa_probe(0x68, &handle) != Tfa98xx_Error_Ok || handle != 0 ||
            (tfa98xx_get_device_revision(handle) & 0xff) != 0x96) {
            result = -ENODEV;
            goto done;
        }
        device->probed = true;
    }
    enum Tfa98xx_Error calibration = calibrate_cold_device();
    if (calibration != Tfa98xx_Error_Ok) {
        ALOGE("Speaker calibration failed: %d", calibration);
        tfa_stop();
        result = -EIO;
        goto done;
    }
    /* Keep profile HQ and volume step zero, as used by the existing wrapper. */
    int steps[1] = {0};
    enum tfa_error error = tfa_start(0, steps);
    if (error != tfa_error_ok) {
        ALOGE("SpeakerBoost startup failed: %d", error);
        tfa_stop();
        result = -EIO;
        goto done;
    }
    device->enabled = true;
    result = 0;
done:
    pthread_mutex_unlock(&lock);
    return result;
}

void tfa9896_close(struct tfa9896 *device) {
    pthread_mutex_lock(&lock);
    if (device && device == owner) {
        if (device->probed && tfa_stop() != tfa_error_ok) {
            ALOGE("Cannot stop amplifier while closing");
        }
        tfa_deinit();
        tfa_i2c_close();
        free(device->container);
        free(device);
        owner = NULL;
    }
    pthread_mutex_unlock(&lock);
}

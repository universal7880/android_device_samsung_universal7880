/*
 * Copyright (C) 2017 Christopher N. Hesse <raymanfx@gmail.com>
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "audio_hw_amplifier_tfa"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <log/log.h>
#include <tinyalsa/asoundlib.h>

#include "tfa.h"

static void *write_dummy_data(void *argument) {
    tfa_device_t *device = argument;
    struct pcm_config config = {
        .channels = 2,
        .rate = 48000,
        .period_size = 256,
        .period_count = 2,
        .format = PCM_FORMAT_S16_LE,
        .start_threshold = 511,
        .stop_threshold = UINT_MAX,
        .avail_min = 1,
    };
    struct pcm *pcm = pcm_open(0, 0, PCM_OUT | PCM_MONOTONIC, &config);
    int error = 0;
    if (!pcm || !pcm_is_ready(pcm)) {
        /* An existing output may already own the PCM. The controller still
         * verifies clock stability before touching DSP configuration. */
        if (pcm && errno == EBUSY) {
            pthread_mutex_lock(&device->mutex);
            device->writing = true;
            pthread_mutex_unlock(&device->mutex);
            goto done;
        }
        ALOGE("Cannot supply amplifier clock: %s", pcm ? pcm_get_error(pcm) : "no PCM");
        error = -ENODEV;
        goto done;
    }
    uint8_t buffer[8192] = {0};
    while (atomic_load(&device->initializing)) {
        if (pcm_write(pcm, buffer, sizeof(buffer))) {
            ALOGE("Cannot write amplifier clock PCM: %s", pcm_get_error(pcm));
            error = -EIO;
            break;
        }
        pthread_mutex_lock(&device->mutex);
        if (!device->writing) {
            device->writing = true;
            pthread_cond_signal(&device->cond);
        }
        pthread_mutex_unlock(&device->mutex);
    }
done:
    pthread_mutex_lock(&device->mutex);
    device->clock_error = error;
    pthread_cond_signal(&device->cond);
    pthread_mutex_unlock(&device->mutex);
    if (pcm) pcm_close(pcm);
    return NULL;
}

static void clock_off(tfa_device_t *device) {
    if (!device->clock_enabled) return;
    atomic_store(&device->initializing, false);
    pthread_join(device->write_thread, NULL);
    device->clock_enabled = false;
}

static int clock_on(tfa_device_t *device) {
    device->writing = false;
    device->clock_error = 0;
    atomic_store(&device->initializing, true);
    int error = pthread_create(&device->write_thread, NULL, write_dummy_data, device);
    if (error) {
        atomic_store(&device->initializing, false);
        return -error;
    }
    device->clock_enabled = true;
    pthread_mutex_lock(&device->mutex);
    while (!device->writing && !device->clock_error) {
        pthread_cond_wait(&device->cond, &device->mutex);
    }
    error = device->clock_error;
    pthread_mutex_unlock(&device->mutex);
    if (error) clock_off(device);
    return error;
}

int tfa_power(tfa_device_t *device, bool on) {
    if (!device) return -EINVAL;
    pthread_mutex_lock(&device->tfa_lock);
    int result = on ? clock_on(device) : 0;
    if (!result) result = tfa9896_enable(device->amplifier, on);
    if (on) {
        clock_off(device);
        if (!result) result = device->clock_error;
        if (result) tfa9896_enable(device->amplifier, false);
    }
    pthread_mutex_unlock(&device->tfa_lock);
    return result;
}

tfa_device_t *tfa_device_open(void) {
    tfa_device_t *device = calloc(1, sizeof(*device));
    if (!device) return NULL;
    device->amplifier = tfa9896_open();
    if (!device->amplifier) {
        free(device);
        return NULL;
    }
    int error = pthread_mutex_init(&device->tfa_lock, NULL);
    if (error) goto fail;
    error = pthread_mutex_init(&device->mutex, NULL);
    if (error) goto fail_lock;
    error = pthread_cond_init(&device->cond, NULL);
    if (error) goto fail_mutex;
    atomic_init(&device->initializing, false);
    return device;
fail_mutex:
    pthread_mutex_destroy(&device->mutex);
fail_lock:
    pthread_mutex_destroy(&device->tfa_lock);
fail:
    tfa9896_close(device->amplifier);
    free(device);
    errno = error;
    return NULL;
}

void tfa_device_close(tfa_device_t *device) {
    if (!device) return;
    tfa_power(device, false);
    tfa9896_close(device->amplifier);
    pthread_cond_destroy(&device->cond);
    pthread_mutex_destroy(&device->mutex);
    pthread_mutex_destroy(&device->tfa_lock);
    free(device);
}

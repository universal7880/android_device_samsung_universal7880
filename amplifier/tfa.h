/*
 * Copyright (C) 2017 Christopher N. Hesse <raymanfx@gmail.com>
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <tfa9896.h>

typedef enum {
    Audio_Mode_None = -1,
    Audio_Mode_Music_Normal,
    Audio_Mode_Voice,
    Audio_Mode_Max
} tfa_mode_t;

typedef struct {
    struct tfa9896 *amplifier;
    pthread_mutex_t tfa_lock;
    atomic_bool initializing;
    bool clock_enabled;
    bool writing;
    int clock_error;
    pthread_t write_thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} tfa_device_t;

int tfa_power(tfa_device_t *device, bool on);
tfa_device_t *tfa_device_open(void);
void tfa_device_close(tfa_device_t *device);

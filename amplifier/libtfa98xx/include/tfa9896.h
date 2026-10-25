/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 The LineageOS Project */
#pragma once

#include <stdbool.h>

struct tfa9896;

/* One instance owns the mono amplifier and the NXP service state. */
struct tfa9896 *tfa9896_open(void);
/* The caller must supply the 48 kHz audio clock until this returns. */
int tfa9896_enable(struct tfa9896 *device, bool enabled);
void tfa9896_close(struct tfa9896 *device);

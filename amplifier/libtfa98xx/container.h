/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 The LineageOS Project */
#pragma once
#include <stddef.h>

/* Validate all reachable records before passing the file to the NXP parser. */
int tfa_container_validate(const void *data, size_t size);

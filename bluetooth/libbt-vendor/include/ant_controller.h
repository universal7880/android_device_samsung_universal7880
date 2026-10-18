/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The library owns the fd. Stop ANT I/O before release; Bluetooth may keep running. */
int ant_controller_acquire(const uint8_t address[6]);
void ant_controller_release(void);
#ifdef __cplusplus
}
#endif

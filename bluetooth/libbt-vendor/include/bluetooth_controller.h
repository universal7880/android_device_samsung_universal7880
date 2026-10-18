/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Borrowed descriptor: stop Bluetooth I/O before release. ANT may keep running. */
int bluetooth_controller_acquire(const uint8_t address[6]);
void bluetooth_controller_release(void);
#ifdef __cplusplus
}
#endif

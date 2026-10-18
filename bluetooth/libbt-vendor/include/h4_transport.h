/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The transport owns the returned fd; stop before closing the UART. */
int h4_transport_start(int uart_fd);
int h4_transport_start_ant(int uart_fd);
/* Channel 0 is Bluetooth, channel 1 is ANT. Caller serializes lifecycle calls. */
int h4_transport_enable(int channel, int enabled);
void h4_transport_stop(void);

#ifdef __cplusplus
}
#endif

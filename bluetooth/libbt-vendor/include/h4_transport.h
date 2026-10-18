/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The transport owns the returned fd; stop before closing the UART. */
int h4_transport_start(int uart_fd);
void h4_transport_stop(void);

#ifdef __cplusplus
}
#endif

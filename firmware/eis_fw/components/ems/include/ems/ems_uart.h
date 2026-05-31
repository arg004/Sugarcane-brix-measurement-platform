// ==============================
// FILE: components/ems/include/ems/ems_uart.h
// ==============================
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_check.h"

#ifdef __cplusplus
extern "C" {
#endif

// UART + IO ----------------------------------------------------
esp_err_t ems_uart_init(void);                 // init UART (Kconfig-driven)
void      ems_io_lock(void);                   // global non-recursive lock
void      ems_io_unlock(void);

// I/O primitives ----------------------------------------------
esp_err_t ems_send_str(const char *s, bool append_lf, int tx_timeout_ms);
int       ems_read_until(char *out, size_t out_cap, const char *delims, int timeout_ms);

// Probe --------------------------------------------------------
esp_err_t ems_probe(char *version_out, size_t out_sz, int timeout_ms);

#ifdef __cplusplus
}
#endif
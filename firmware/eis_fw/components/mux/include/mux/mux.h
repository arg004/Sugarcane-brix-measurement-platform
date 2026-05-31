// =============================================================
// FILE: components/mux/include/mux/mux.h
// =============================================================
#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mux_init(void);              // init GPIOs and park SAFE
esp_err_t mux_select(uint8_t sensor);  // 0=SAFE(11), 1=00, 2=01, 3=10

#ifdef __cplusplus
}
#endif
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Drive GPIOs to safe HIGH (inactive). Call as early as possible after boot. */
void regen_gpio_safe_boot_init(void);

/** Initialize queues/tasks; does **not** start MQTT. Safe to call early. */
esp_err_t regen_init(void);

/** Attach to the shared MQTT client once it exists (after network/heartbeat). */
esp_err_t regen_attach_mqtt(void);

/** Enqueue a single‑sensor pulse. duration_ms=0 → default from Kconfig. */
esp_err_t regen_request(uint8_t sensor, uint32_t duration_ms, uint32_t *job_id_out);

/** Enqueue a DI‑wash pulse (all three pins simultaneously). */
esp_err_t regen_request_all(uint32_t duration_ms, uint32_t *job_id_out);

/** Not supported (pulses are atomic). */
esp_err_t regen_cancel_current(void);

#ifdef __cplusplus
}
#endif
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize queues, task, MQTT topics (does NOT start MQTT itself)
esp_err_t measure_init(void);

// Attach to the shared MQTT client (call after network/heartbeat bring up)
esp_err_t measure_attach_mqtt(void);

// Enqueue an immediate measurement job (synthetic data for now)
esp_err_t measure_request(uint8_t sensor, uint32_t num_points, uint32_t *job_id_out);

bool      measure_is_busy(void);

#ifdef __cplusplus
}
#endif
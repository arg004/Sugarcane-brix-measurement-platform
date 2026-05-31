// ====================================================================
// FILE: components/ems/include/ems/ems_eis.h
// ====================================================================
#pragma once

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize EIS runner (topics, MQTT handle, single-flight lock).
 */
esp_err_t ems_eis_init(void);

/**
 * Handle MQTT JSON for EIS run.
 * Accepts: {"op":"eis.run"|"eis", "sensor":1..3}
 * Returns ESP_ERR_NOT_FOUND if op is not EIS.
 */
esp_err_t ems_eis_handle_mqtt(cJSON *root);

#ifdef __cplusplus
}
#endif
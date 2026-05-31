#pragma once
#include "mqtt_client.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" { 
#endif
// why: expose single MQTT client for all modules (heartbeat, regen, etc.)
esp_mqtt_client_handle_t telemetry_mqtt_get_client(void);
// start the client if not already running (uses heartbeat's config)
esp_err_t telemetry_mqtt_ensure_started(void);
#ifdef __cplusplus
}
#endif
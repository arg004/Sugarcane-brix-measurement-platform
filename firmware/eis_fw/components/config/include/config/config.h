// firmware/components/config/include/config/config.h (new)
#pragma once
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t config_init(void);
esp_err_t config_get_device_id(char *out, size_t out_len);

// Generic helpers (for future keys like wifi_ssid, server_url)
esp_err_t config_set_str(const char *key, const char *val);
esp_err_t config_get_str(const char *key, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
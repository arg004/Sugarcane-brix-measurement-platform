// firmware/components/network/include/network/wifi.h  (new)
#pragma once
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_start_station(void);
esp_err_t wifi_wait_ipv4(uint32_t timeout_ms, esp_netif_ip_info_t *out_ip);

#ifdef __cplusplus
}
#endif
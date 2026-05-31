// firmware/components/network/wifi.c  (update: idempotent init)
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include "network/wifi.h"

#ifndef CONFIG_APP_WIFI_SSID
#define CONFIG_APP_WIFI_SSID ""
#endif
#ifndef CONFIG_APP_WIFI_PASS
#define CONFIG_APP_WIFI_PASS ""
#endif

static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_netif = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_GOT_IP_BIT    BIT1

static void on_wifi_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);
        esp_wifi_connect();
        ESP_LOGW(TAG, "Disconnected; reconnecting...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_start_station(void) {
    // why: ensure subsystems exist, but don’t abort if already created by NetworkTask
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));

    wifi_config_t sta = {0};
    snprintf((char*)sta.sta.ssid, sizeof(sta.sta.ssid), "%s", CONFIG_APP_WIFI_SSID);
    snprintf((char*)sta.sta.password, sizeof(sta.sta.password), "%s", CONFIG_APP_WIFI_PASS);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID='%s'", CONFIG_APP_WIFI_SSID);
    return ESP_OK;
}

esp_err_t wifi_wait_ipv4(uint32_t timeout_ms, esp_netif_ip_info_t *out_ip) {
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_GOT_IP_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & WIFI_GOT_IP_BIT)) return ESP_ERR_TIMEOUT;
    if (out_ip && s_netif) esp_netif_get_ip_info(s_netif, out_ip);
    return ESP_OK;
}
// firmware/components/network/network_task.c  (new)
#include <stdio.h>
#include "esp_log.h"
#include "esp_event.h"            // event base/loop
#include "esp_wifi.h"             // WIFI_EVENT + IDs
#include "esp_netif.h"            // IP_EVENT + IP_EVENT_STA_GOT_IP
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "network/network_task.h"
#include "network/wifi.h"
#include "network/sntp_time.h"

static const char *TAG = "NETTASK";

// Public events visible to other components
static EventGroupHandle_t s_events;
// Internal events (hidden): used to wake the task on link‑down
static EventGroupHandle_t s_int_events;
#define INT_EVT_LINK_DOWN  BIT8   // internal only

static TaskHandle_t s_task;

// why: event handler updates public bits immediately on link changes
static void on_wifi_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // what: connection lost → clear usable state and notify task to re‑sync
        xEventGroupClearBits(s_events, NET_EVT_WIFI_IP | NET_EVT_TIME_SYNCED);
        xEventGroupSetBits(s_int_events, INT_EVT_LINK_DOWN);
        ESP_LOGW(TAG, "Wi‑Fi disconnected; clearing IP/TIME bits");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        // what: we have IPv4 now → mark network usable
        xEventGroupSetBits(s_events, NET_EVT_WIFI_IP);
    }
}

static void task_fn(void *arg) {
    xEventGroupSetBits(s_events, NET_EVT_WIFI_READY); // why: let others proceed with lazy init

    // Start Wi‑Fi once; reconnect policy lives in wifi.c
    wifi_start_station();

    // why: capped exponential backoff avoids busy‑wait and AP hammering
    const uint32_t max_backoff_ms = 30000;
    for (;;) {
        uint32_t backoff = 1000;
        // wait for IPv4 address
        while ((xEventGroupGetBits(s_events) & NET_EVT_WIFI_IP) == 0) {
            if (wifi_wait_ipv4(10000, NULL) == ESP_OK) break; // what: helper blocks up to 10s
            ESP_LOGW(TAG, "No IP yet; retry in %u ms", backoff);
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff = (backoff < max_backoff_ms) ? backoff * 2 : max_backoff_ms;
        }

        // start SNTP and wait for a usable clock
        sntp_start(CONFIG_APP_TIMEZONE); // how: uses TZ, smooth sync
        if (sntp_wait_for_sync(20000)) {
            xEventGroupSetBits(s_events, NET_EVT_TIME_SYNCED);
            char buf[40];
            sntp_format_time_now(buf, sizeof buf);
            ESP_LOGI(TAG, "Time synced: %s", buf);
        } else {
            ESP_LOGW(TAG, "SNTP not synced within timeout");
        }

        // block until we detect a link‑down, then loop to reacquire
        xEventGroupWaitBits(s_int_events, INT_EVT_LINK_DOWN, pdTRUE, pdFALSE, portMAX_DELAY);
        // why: after wake → loop restarts to reacquire IP and re‑sync time
    }
}

void network_task_start(void) {
    if (!s_events) s_events = xEventGroupCreate();
    if (!s_int_events) s_int_events = xEventGroupCreate();

    // ensure event loop exists before registering handlers
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

    // subscribe once
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_ip_event, NULL));

    if (!s_task) {
        // modest stack, medium prio; network waits must not starve other tasks
        xTaskCreate(task_fn, "network", 4096, NULL, 4, &s_task);
    }
}

EventGroupHandle_t network_events(void) { return s_events; }

bool network_get_time(char *buf, size_t len) {
    if (!buf || len < 2) return false; // guard
    sntp_format_time_now(buf, len);
    return true;
}
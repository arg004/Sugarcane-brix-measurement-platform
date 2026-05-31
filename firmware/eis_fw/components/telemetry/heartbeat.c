// firmware/components/telemetry/heartbeat.c  (new: MQTT transport)
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"          // uptime
#include "esp_system.h"
#include "esp_wifi.h"            // RSSI via esp_wifi_sta_get_ap_info()
#include "esp_netif.h"           // netif handle
#include "esp_netif_ip_addr.h"   // IP2STR macro
#include "mqtt_client.h"         // MQTT client
#include "cJSON.h"               // JSON payload
#include "esp_random.h"          // jitter
#include "esp_ota_ops.h"         // fw version/sha
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "network/network_task.h" // sync with NET_EVT bits
#include "config/config.h"        // device_id helper
#include "telemetry/mqtt_bus.h"
#include "esp_app_desc.h"   // for esp_app_get_description (deprec. fix)

static const char *TAG = "HBMQTT";

#ifndef CONFIG_APP_HB_MQTT_URI
#define CONFIG_APP_HB_MQTT_URI "mqtt://test.mosquitto.org:1883"  // safe default
#endif
#ifndef CONFIG_APP_HB_PERIOD_SEC
#define CONFIG_APP_HB_PERIOD_SEC 300
#endif
#ifndef CONFIG_APP_HB_QOS
#define CONFIG_APP_HB_QOS 0
#endif
#ifndef CONFIG_APP_HB_KEEPALIVE
#define CONFIG_APP_HB_KEEPALIVE 60
#endif
#ifndef CONFIG_APP_HB_TOPIC_PREFIX
#define CONFIG_APP_HB_TOPIC_PREFIX "devices"
#endif

// optional user KV (guarded by simple copy in task context)
static char s_extra_k[24] = {0};
static char s_extra_v[64] = {0};

static TaskHandle_t s_task;
static esp_mqtt_client_handle_t s_client;
static EventGroupHandle_t s_mqtt_ev;
#define MQTT_EV_CONNECTED BIT0

// persistent topic buffers (must outlive client config)
static char s_status_topic[128] = {0};
static char s_hb_topic[128] = {0};
static char s_will_payload[64] = {0};
static char s_online_payload[64] = {0};

static void get_fw_info(const char **out_ver, const char **out_proj, char *sha_hex, size_t sha_len) {
    // what: read current app metadata; why: include version/sha for fleet visibility
    // OLD: const esp_app_desc_t *d = esp_ota_get_app_description();
    const esp_app_desc_t *d = esp_app_get_description();
    if (out_ver) *out_ver = d ? d->version : "1";
    if (out_proj) *out_proj = d ? d->project_name : "app";
    if (sha_hex && sha_len) {
        size_t n = 0;
        if (d) {
            for (size_t i = 0; i < sizeof d->app_elf_sha256 && (n + 2) < sha_len; ++i) {
                n += snprintf(&sha_hex[n], sha_len - n, "%02x", (unsigned)(uint8_t)d->app_elf_sha256[i]);
            }
        }
        if (n < sha_len) sha_hex[n] = '\0';
    }
}

static int get_rssi(void) {
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

static void format_ip(char *buf, size_t len) {
    if (!buf || len < 8) return;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &info) == ESP_OK) {
        snprintf(buf, len, "%d.%d.%d.%d", IP2STR(&info.ip));
    } else {
        buf[0] = '\0';
    }
}

static bool build_payload(char *device_id, char **out_body) {
    char ts[40] = {0};
    bool have_time = false;
    if ((xEventGroupGetBits(network_events()) & NET_EVT_TIME_SYNCED) != 0) {
        char tbuf[40];
        if (network_get_time(tbuf, sizeof tbuf)) { strncpy(ts, tbuf, sizeof ts - 1); have_time = true; }
    }
    const char *ver = NULL, *proj = NULL; char sha[65] = {0};
    get_fw_info(&ver, &proj, sha, sizeof sha);

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "fw_ver", ver);
    cJSON_AddStringToObject(root, "fw_proj", proj);
    cJSON_AddStringToObject(root, "fw_sha", sha);
    if (have_time) cJSON_AddStringToObject(root, "ts", ts);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time()/1000));
    cJSON_AddNumberToObject(root, "rssi", get_rssi());
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    char ip[32]; format_ip(ip, sizeof ip); if (ip[0]) cJSON_AddStringToObject(root, "ip", ip);
    if (s_extra_k[0]) cJSON_AddStringToObject(root, s_extra_k, s_extra_v);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    *out_body = printed;
    return printed != NULL;
}

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            xEventGroupSetBits(s_mqtt_ev, MQTT_EV_CONNECTED);
            // what: publish retained online status; why: broker shows presence
            esp_mqtt_client_publish(s_client, s_status_topic, s_online_payload, 0, CONFIG_APP_HB_QOS, 1);
            ESP_LOGI(TAG, "MQTT connected; status=online published");
            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(s_mqtt_ev, MQTT_EV_CONNECTED);
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT error type=%d", e->error_handle->error_type);
            break;
        default:
            break;
    }
}

static esp_err_t mqtt_start_client(const char *device_id) {
    // build topics/payloads once (must persist)
    snprintf(s_status_topic, sizeof s_status_topic, "%s/%s/status", CONFIG_APP_HB_TOPIC_PREFIX, device_id);
    snprintf(s_hb_topic, sizeof s_hb_topic, "%s/%s/heartbeat", CONFIG_APP_HB_TOPIC_PREFIX, device_id);
    snprintf(s_will_payload, sizeof s_will_payload, "{\"state\":\"offline\"}");
    snprintf(s_online_payload, sizeof s_online_payload, "{\"state\":\"online\"}");

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = CONFIG_APP_HB_MQTT_URI;               // how: v5.5 API
    cfg.session.keepalive = CONFIG_APP_HB_KEEPALIVE;               // why: predictable ping
    cfg.session.last_will.topic = s_status_topic;                  // how: LWT retained offline
    cfg.session.last_will.msg = s_will_payload;
    cfg.session.last_will.qos = CONFIG_APP_HB_QOS;
    cfg.session.last_will.retain = 1;
    // leave client_id NULL → defaults to ESP32_%CHIPID%; topics carry device_id

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    return esp_mqtt_client_start(s_client);
}

static void task_fn(void *arg) {
    // what: wait for network usability
    xEventGroupWaitBits(network_events(), NET_EVT_WIFI_IP, pdFALSE, pdFALSE, portMAX_DELAY);

    char device_id[64] = {0};
    if (config_get_device_id(device_id, sizeof device_id) != ESP_OK) strcpy(device_id, "unknown");

    s_mqtt_ev = xEventGroupCreate();
    if (mqtt_start_client(device_id) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed");
        vTaskDelete(NULL);
        return;
    }

    // wait for MQTT session
    xEventGroupWaitBits(s_mqtt_ev, MQTT_EV_CONNECTED, pdFALSE, pdFALSE, portMAX_DELAY);

    const int32_t base_ms = (CONFIG_APP_HB_PERIOD_SEC > 5 ? CONFIG_APP_HB_PERIOD_SEC : 5) * 1000; // base period in ms
uint32_t backoff_ms = 1000;

    for (;;) {
        // jitter: ±10% (signed!) to avoid herd effects; guard against unsigned wrap
int32_t jitter_ms = (int32_t)(esp_random() % (base_ms / 5)) - (int32_t)(base_ms / 10); // ±10%
int32_t sleep_ms = base_ms + jitter_ms;
if (sleep_ms < 100) sleep_ms = 100; // why: never sleep negative/too small
TickType_t sleep_ticks = pdMS_TO_TICKS((uint32_t)sleep_ms);

        char *body = NULL;
        if (!build_payload(device_id, &body)) {
            ESP_LOGE(TAG, "payload alloc failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // publish heartbeat (non-retained), QoS configurable
        int mid = esp_mqtt_client_publish(s_client, s_hb_topic, body, 0, CONFIG_APP_HB_QOS, 0);
        cJSON_free(body);

        if (mid >= 0) {
            backoff_ms = 1000; // reset on success enqueue
            // how: simple relative delay; avoids vTaskDelayUntil catch-up after long disconnects
            vTaskDelay(sleep_ticks);
        } else {
            ESP_LOGW(TAG, "publish failed; retry in %u ms", backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms < 30000) ? backoff_ms * 2 : 30000;
        }

        // if link or MQTT is down, wait again
        if ((xEventGroupGetBits(network_events()) & NET_EVT_WIFI_IP) == 0) {
            xEventGroupWaitBits(network_events(), NET_EVT_WIFI_IP, pdFALSE, pdFALSE, portMAX_DELAY);
        }
        if ((xEventGroupGetBits(s_mqtt_ev) & MQTT_EV_CONNECTED) == 0) {
            xEventGroupWaitBits(s_mqtt_ev, MQTT_EV_CONNECTED, pdFALSE, pdFALSE, portMAX_DELAY);
        }
    }
}

bool heartbeat_start(void) {
    if (s_task) return true; // idempotent
    if (!CONFIG_APP_HB_MQTT_URI[0]) { ESP_LOGW(TAG, "heartbeat disabled: empty MQTT URI"); return false; }
    // OLD: if (xTaskCreate(task_fn, "hb_mqtt", 4096, NULL, 3, &s_task) == NULL) return false;
    if (xTaskCreate(task_fn, "hb_mqtt", 4096, NULL, 3, &s_task) != pdPASS) return false; // why: pdPASS is the success code
    return true;
}

bool heartbeat_set_extra_kv(const char *k, const char *v) {
    if (!k || !v) return false;
    strncpy(s_extra_k, k, sizeof s_extra_k - 1);
    strncpy(s_extra_v, v, sizeof s_extra_v - 1);
    return true;
}

esp_mqtt_client_handle_t telemetry_mqtt_get_client(void) {
    return s_client; // why: allow other modules to reuse connection
}

esp_err_t telemetry_mqtt_ensure_started(void) {
    if (s_client) return ESP_OK;
    esp_mqtt_client_config_t cfg = (esp_mqtt_client_config_t){ 0 };
#ifdef CONFIG_APP_HB_MQTT_URI
    if (CONFIG_APP_HB_MQTT_URI[0]) cfg.broker.address.uri = CONFIG_APP_HB_MQTT_URI;
#endif
    if (!cfg.broker.address.uri) cfg.broker.address.uri = "mqtt://test.mosquitto.org:1883"; // dev default
    cfg.session.keepalive = 60;
    cfg.session.disable_clean_session = false;   // why: keep subs on reconnect
    cfg.network.reconnect_timeout_ms = 2000;     // why: fast retry; esp-mqtt backoff still applies

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    // Note: modules (heartbeat, regen, etc.) must register their own event handlers
    // after calling ensure_started(). Keeping bus generic avoids symbol coupling.
    esp_mqtt_client_start(s_client);
    return ESP_OK;
}
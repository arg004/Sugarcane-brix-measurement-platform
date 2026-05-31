#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>       // strcasecmp
#include "sdkconfig.h"     // Kconfig symbols
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "esp_event.h"
#include "cJSON.h"
#include "config/config.h"     // device_id helper
#include "regen/regen_ctrl.h"  // public API
#include "telemetry/mqtt_bus.h" // reuse shared MQTT client & reconnect logic

static const char *TAG = "REGEN";

// ---- GPIO & timing config (from Kconfig, with sane fallbacks) ----
#ifndef CONFIG_APP_REGEN_GPIO_1
#define CONFIG_APP_REGEN_GPIO_1 21
#endif
#ifndef CONFIG_APP_REGEN_GPIO_2
#define CONFIG_APP_REGEN_GPIO_2 47
#endif
#ifndef CONFIG_APP_REGEN_GPIO_3
#define CONFIG_APP_REGEN_GPIO_3 48
#endif
#ifndef CONFIG_APP_REGEN_ACTIVE_HIGH
#define CONFIG_APP_REGEN_ACTIVE_HIGH 0   // default active‑LOW
#endif
#ifndef CONFIG_APP_REGEN_DEFAULT_MS
#define CONFIG_APP_REGEN_DEFAULT_MS 1000
#endif
#ifndef CONFIG_APP_REGEN_WASH_ENABLE
#define CONFIG_APP_REGEN_WASH_ENABLE 1
#endif
#ifndef CONFIG_APP_REGEN_WASH_MS
#define CONFIG_APP_REGEN_WASH_MS 1000
#endif
#ifndef CONFIG_APP_REGEN_COOLDOWN_MS
#define CONFIG_APP_REGEN_COOLDOWN_MS 5000
#endif

static int s_gpio[3] = {
    CONFIG_APP_REGEN_GPIO_1,
    CONFIG_APP_REGEN_GPIO_2,
    CONFIG_APP_REGEN_GPIO_3,
};

static inline int level_on(void)  { return CONFIG_APP_REGEN_ACTIVE_HIGH ? 1 : 0; }
static inline int level_off(void) { return CONFIG_APP_REGEN_ACTIVE_HIGH ? 0 : 1; }

// ---- MQTT topics / broker ----
static esp_mqtt_client_handle_t s_client;
static char s_topic_cmd[160];
static char s_topic_status[160];
static char s_device_id[64];

// ---- Job & runtime state ----
typedef struct {
    uint32_t job_id;
    uint8_t  mask;         // bit0→pin1, bit1→pin2, bit2→pin3
    uint32_t duration_ms;
    bool     is_wash;
} regen_req_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_active_mtx;  // serialize pulses
static TaskHandle_t s_task;
static uint32_t s_next_job_id = 1;

// ---- Helpers ----
static void gpio_init_safe(void) {
    // ensure outputs exist and default to safe (inactive) state on boot
    for (int i = 0; i < 3; ++i) {
        if (s_gpio[i] < 0) continue;                 // allow disabling via -1
        if (s_gpio[i] > 48) {                         // bound check for S3
            ESP_LOGE(TAG, "invalid GPIO %d for regen", s_gpio[i]);
            continue;
        }
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << s_gpio[i]),
            .mode = GPIO_MODE_OUTPUT_OD, // to make compatible with regen system
            .pull_up_en = 0,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t r = gpio_config(&io);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config(%d) failed: %s", s_gpio[i], esp_err_to_name(r));
            continue;
        }
        gpio_set_level(s_gpio[i], level_off());  // inactive on boot (HIGH if active‑LOW)
    }
}

void regen_gpio_safe_boot_init(void) {
    // why: allow safe pin drive before network/MQTT exists
    gpio_init_safe();
}

static void set_mask(uint8_t mask, bool on) {
    for (int i = 0; i < 3; ++i) {
        if ((mask & (1u << i)) && s_gpio[i] >= 0) {
            gpio_set_level(s_gpio[i], on ? level_on() : level_off());
        }
    }
}

static void status_publish(const char *event, const regen_req_t *r, const char *reason) {
    if (!s_client) return;
    char json[192];
    const char *sensor_str = r->is_wash ? "\"all\""
                           : (r->mask & 0x1) ? "\"1\""
                           : (r->mask & 0x2) ? "\"2\"" : "\"3\"";
    int n = snprintf(json, sizeof json,
                     "{\"event\":\"%s\",\"job_id\":%u,\"sensor\":%s,\"duration_ms\":%u%s%s}",
                     event,
                     (unsigned)r->job_id,
                     sensor_str,
                     (unsigned)r->duration_ms,
                     reason?",\"reason\":\"":"",
                     reason?reason:"");
    (void)n;
    esp_mqtt_client_publish(s_client, s_topic_status, json, 0, 1, 0);
}

// ---- MQTT parsing ----
static int json_get_int(const cJSON *root, const char *key, int def) {
    const cJSON *v = cJSON_GetObjectItem(root, key);
    return (v && cJSON_IsNumber(v)) ? v->valueint : def;
}
static bool json_get_bool(const cJSON *root, const char *key, bool def) {
    const cJSON *v = cJSON_GetObjectItem(root, key);
    if (!v) return def;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return def;
}
static bool is_all_sensor(const cJSON *sensor_item) {
    if (!sensor_item) return false;
    if (cJSON_IsNumber(sensor_item)) return (sensor_item->valueint == 0);
    if (cJSON_IsString(sensor_item) && sensor_item->valuestring)
        return strcasecmp(sensor_item->valuestring, "all") == 0;
    return false;
}

static void handle_cmd_payload(const char *data, int len) {
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) { ESP_LOGW(TAG, "bad JSON"); return; }

    const cJSON *sensor_item = cJSON_GetObjectItem(root, "sensor");
    bool wash = json_get_bool(root, "wash", false) || is_all_sensor(sensor_item);
    int pulse_ms = json_get_int(root, "pulse_ms", CONFIG_APP_REGEN_DEFAULT_MS);
    if (pulse_ms <= 0) pulse_ms = CONFIG_APP_REGEN_DEFAULT_MS;

    esp_err_t res = ESP_FAIL; uint32_t job = 0;
    if (wash) {
    #if CONFIG_APP_REGEN_WASH_ENABLE
        res = regen_request_all((uint32_t)pulse_ms, &job);
    #else
        res = ESP_ERR_NOT_SUPPORTED;
    #endif
    } else if (sensor_item && cJSON_IsNumber(sensor_item)) {
        int s = sensor_item->valueint;
        res = regen_request((uint8_t)s, (uint32_t)pulse_ms, &job);
    } else {
        res = ESP_ERR_INVALID_ARG;
    }

    regen_req_t tmp = {
        .job_id = job,
        .mask = wash ? 0x7 : (sensor_item && cJSON_IsNumber(sensor_item) ? (1u << (sensor_item->valueint - 1)) : 0),
        .duration_ms = (uint32_t)pulse_ms,
        .is_wash = wash
    };
    if (res == ESP_OK) status_publish("accepted", &tmp, NULL);
    else if (res == ESP_ERR_INVALID_ARG) status_publish("rejected", &tmp, "invalid_sensor");
    else if (res == ESP_ERR_NO_MEM) status_publish("rejected", &tmp, "queue_full");
    else if (res == ESP_ERR_INVALID_STATE) status_publish("rejected", &tmp, "wash_requires_all_pins");
    else status_publish("rejected", &tmp, "error");

    cJSON_Delete(root);
}

static void mqtt_evt(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        esp_mqtt_client_subscribe(e->client, s_topic_cmd, 1);
        ESP_LOGI(TAG, "MQTT connected; subscribed %s", s_topic_cmd);
        break;
    case MQTT_EVENT_DATA:
        if (e->topic && (size_t)e->topic_len == strlen(s_topic_cmd) &&
            strncmp(e->topic, s_topic_cmd, (size_t)e->topic_len) == 0) {
            handle_cmd_payload(e->data, e->data_len);
        }
        break;
    default:
        break;
    }
}

// ---- Task (serialize pulses) ----
static void regen_task(void *arg) {
    const TickType_t cooldown = pdMS_TO_TICKS(CONFIG_APP_REGEN_COOLDOWN_MS);
    regen_req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_active_mtx, portMAX_DELAY);
        // atomic pulse; active level then back to safe
        set_mask(r.mask, true);
        vTaskDelay(pdMS_TO_TICKS(r.duration_ms));
        set_mask(r.mask, false);
        status_publish("completed", &r, NULL);
        xSemaphoreGive(s_active_mtx);
        vTaskDelay(cooldown);
    }
}

// ---- Public API ----
esp_err_t regen_init(void) {
    if (config_get_device_id(s_device_id, sizeof s_device_id) != ESP_OK)
        strcpy(s_device_id, "unknown");

    gpio_init_safe(); // important: safe levels at boot

    if (!s_q) s_q = xQueueCreate(8, sizeof(regen_req_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (!s_active_mtx) s_active_mtx = xSemaphoreCreateMutex();
    if (!s_active_mtx) return ESP_ERR_NO_MEM;
    if (!s_task) {
        if (xTaskCreate(regen_task, "regen", 3072, NULL, 4, &s_task) != pdPASS)
            return ESP_ERR_NO_MEM; // run pulse worker
    }

    // Build topics: <prefix>/<id>/commands/regen and <prefix>/<id>/status/regen
    #ifdef CONFIG_APP_HB_TOPIC_PREFIX
        const char *prefix = CONFIG_APP_HB_TOPIC_PREFIX;
    #else
        #ifdef CONFIG_APP_REGEN_TOPIC_PREFIX
            const char *prefix = CONFIG_APP_REGEN_TOPIC_PREFIX;
        #else
            const char *prefix = "devices";
        #endif
    #endif
    snprintf(s_topic_cmd, sizeof s_topic_cmd, "%s/%s/commands/regen", prefix, s_device_id);
    snprintf(s_topic_status, sizeof s_topic_status, "%s/%s/status/regen",   prefix, s_device_id);

    // Attach to MQTT only if the bus already exists. Do **not** start MQTT here.
    s_client = telemetry_mqtt_get_client();
    if (s_client) {
        esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    }

    ESP_LOGI(TAG, "regen ready; cmd=%s status=%s (mqtt_attached=%s)", s_topic_cmd, s_topic_status, s_client?"yes":"no");
    return ESP_OK;
}

esp_err_t regen_attach_mqtt(void) {
    esp_mqtt_client_handle_t c = telemetry_mqtt_get_client();
    if (!c) return ESP_ERR_INVALID_STATE; // call after heartbeat/network started
    if (s_client != c) s_client = c;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    ESP_LOGI(TAG, "regen attached to MQTT; subscribed on connect to %s", s_topic_cmd);
    return ESP_OK;
}

esp_err_t regen_request(uint8_t sensor, uint32_t duration_ms, uint32_t *job_id_out) {
    if (sensor < 1 || sensor > 3) return ESP_ERR_INVALID_ARG;
    regen_req_t r = {
        .job_id = ++s_next_job_id,
        .mask = (1u << (sensor - 1)),
        .duration_ms = duration_ms ? duration_ms : CONFIG_APP_REGEN_DEFAULT_MS,
        .is_wash = false,
    };
    if (xQueueSend(s_q, &r, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    if (job_id_out) *job_id_out = r.job_id;
    status_publish("started", &r, NULL);
    return ESP_OK;
}

esp_err_t regen_request_all(uint32_t duration_ms, uint32_t *job_id_out) {
#if CONFIG_APP_REGEN_WASH_ENABLE
    if (s_gpio[0] < 0 || s_gpio[1] < 0 || s_gpio[2] < 0) return ESP_ERR_INVALID_STATE;
    regen_req_t r = {
        .job_id = ++s_next_job_id,
        .mask = 0x7,
        .duration_ms = duration_ms ? duration_ms : CONFIG_APP_REGEN_WASH_MS,
        .is_wash = true,
    };
    if (xQueueSend(s_q, &r, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    if (job_id_out) *job_id_out = r.job_id;
    status_publish("started", &r, NULL);
    return ESP_OK;
#else
    (void)duration_ms; (void)job_id_out; return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t regen_cancel_current(void) {
    // Not supported (atomic pulse)
    return ESP_ERR_NOT_SUPPORTED;
}
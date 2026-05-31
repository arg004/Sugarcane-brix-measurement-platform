#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>   // free()

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "config/config.h"        // device_id
#include "storage/sdcard.h"       // sdcard_is_mounted/mount/unmount
#include "telemetry/mqtt_bus.h"   // telemetry_mqtt_get_client()
#include "ems/ems_uart.h"

static const char *TAG = "MEAS";

// Topics: <prefix>/<id>/(commands|status)/measure
static char s_topic_cmd[160];
static char s_topic_status[160];
static char s_device_id[64];
static esp_mqtt_client_handle_t s_client;

// Job queue & state
typedef struct {
    uint32_t job_id;
    uint8_t  sensor;         // 1..3 (later)
    uint32_t points;         // number of rows to generate
} job_t;

static QueueHandle_t s_q;
static TaskHandle_t  s_task;
static uint32_t      s_next_job_id = 1;
static volatile bool s_busy = false;

static const char *topic_prefix(void)
{
#ifdef CONFIG_APP_HB_TOPIC_PREFIX
    return CONFIG_APP_HB_TOPIC_PREFIX;  // reuse if set
#else
    return CONFIG_APP_MEAS_TOPIC_PREFIX;
#endif
}

static void ensure_dir(const char *path)
{
    struct stat st; if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return;
    int r = mkdir(path, 0777);
    if (r != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: %d", path, errno);
    }
}

static void publish_status(const char *event, const job_t *j, const char *filepath, const char *reason)
{
    if (!s_client) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddNumberToObject(root, "job_id",  (double)j->job_id);
    cJSON_AddNumberToObject(root, "sensor",  (double)j->sensor);
    cJSON_AddNumberToObject(root, "points",  (double)j->points);
    if (filepath) cJSON_AddStringToObject(root, "file", filepath);
    if (reason)   cJSON_AddStringToObject(root, "reason", reason);

    char *txt = cJSON_PrintUnformatted(root);
    if (txt) {
        esp_mqtt_client_publish(s_client, s_topic_status, txt, 0, 1, 0);
        free(txt);
    }
    cJSON_Delete(root);
}

static void build_filepath(char *out, size_t out_sz, const job_t *j)
{
    // Pattern: <DATA_DIR>/<device>_S<sensor>_<YYYYMMDDThhmmss>_<job>.csv
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%Y%m%dT%H%M%S", &tm);

    const char *dir = CONFIG_APP_MEAS_DATA_DIR;
    ensure_dir(dir);
    snprintf(out, out_sz, "%s/%s_S%u_%s_%06u.csv", dir, s_device_id, (unsigned)j->sensor, ts, (unsigned)j->job_id);
}

// 8.3 fallback builder (works even if FATFS long filenames are disabled)
static void build_short_83(char *out, size_t out_sz, const job_t *j)
{
    // S<sensor><5-digit job>.CSV  →  up to 8 chars + ".CSV"
    unsigned n = j->job_id % 100000;
    snprintf(out, out_sz, "%s/S%u%05u.CSV", CONFIG_APP_MEAS_DATA_DIR, (unsigned)j->sensor, n);
}

static void write_synthetic_csv(FILE *f, uint32_t points)
{
    // CSV header
    fprintf(f, "freq, Z', Z''\n");

    // Simple synthetic sweep
    double f0 = 100.0, f1 = 10000.0;
    for (uint32_t i = 0; i < points; ++i) {
        double frac = (points == 1) ? 0.0 : (double)i / (double)(points - 1);
        double freq = f0 + (f1 - f0) * frac;
        double zre  = 100.0 + 0.1  * (double)i;
        double zim  = -50.0 + 0.05 * (double)i;
        fprintf(f, "%.3f, %.6f, %.6f\n", freq, zre, zim);
    }
}

static void task_fn(void *arg)
{
    (void)arg; job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) continue;
        s_busy = true;
        if (!sdcard_is_mounted()) {
            publish_status("rejected", &j, NULL, "sd_not_mounted");
            s_busy = false; continue;
        }

        // Build preferred long filename
        char path[256]; build_filepath(path, sizeof path, &j);

        // Open file first. Only publish "started" after we have a handle.
        FILE *f = fopen(path, "w");
        if (!f) {
            // If long filenames are disabled, FatFs returns EINVAL
            if (errno == EINVAL) {
                char shortp[96]; build_short_83(shortp, sizeof shortp, &j);
                f = fopen(shortp, "w");
                if (!f) {
                    char why[40]; snprintf(why, sizeof why, "fopen_failed:%d", errno);
                    publish_status("failed", &j, shortp, why);
                    s_busy = false; continue;
                }
                publish_status("started", &j, shortp, NULL);
            } else {
                char why[40]; snprintf(why, sizeof why, "fopen_failed:%d", errno);
                publish_status("failed", &j, path, why);
                s_busy = false; continue;
            }
        } else {
            publish_status("started", &j, path, NULL);
        }

        // ... inside on_cmd_payload() or just before writing the CSV:
        char ver[128] = {0};
        esp_err_t pr = ems_probe(ver, sizeof ver, 1500);
        if (pr != ESP_OK) {
            // publish a failure status using your existing helper
            // reason: "ems_no_reply"
            publish_status("failed", &j, NULL, "ems_no_reply");
            return; // do not enqueue / do not write CSV
        }
        // Optional: log (or include in status) the version string
        ESP_LOGI("MEAS", "EmStat probe OK: %s", ver);

        // Write synthetic data now
        write_synthetic_csv(f, j.points);
        fflush(f);
        fclose(f);
        publish_status("completed", &j, path, NULL);
        s_busy = false;
    }
}

static int json_get_int(const cJSON *root, const char *key, int def)
{ const cJSON *v = cJSON_GetObjectItem(root, key); return (v && cJSON_IsNumber(v)) ? v->valueint : def; }

static void on_cmd_payload(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) { ESP_LOGW(TAG, "bad JSON"); return; }

    int sensor = json_get_int(root, "sensor", 1);
    int points = json_get_int(root, "points", 100);
    if (points <= 0) points = 1;
    if ((uint32_t)points > CONFIG_APP_MEAS_MAX_POINTS) points = CONFIG_APP_MEAS_MAX_POINTS;

    job_t j = { .job_id = ++s_next_job_id, .sensor = (uint8_t)sensor, .points = (uint32_t)points };
    esp_err_t r = (xQueueSend(s_q, &j, 0) == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
    if (r == ESP_OK) publish_status("accepted", &j, NULL, NULL);
    else publish_status("rejected", &j, NULL, "queue_full");

    cJSON_Delete(root);
}

static void mqtt_evt(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        esp_mqtt_client_subscribe(e->client, s_topic_cmd, 1);
        ESP_LOGI(TAG, "MQTT connected; subscribed %s", s_topic_cmd);
        break;
    case MQTT_EVENT_DATA:
        if (e->topic && strncmp(e->topic, s_topic_cmd, (size_t)e->topic_len) == 0) {
            on_cmd_payload(e->data, e->data_len);
        }
        break;
    default: break;
    }
}

esp_err_t measure_init(void)
{
    if (config_get_device_id(s_device_id, sizeof s_device_id) != ESP_OK)
        strcpy(s_device_id, "unknown");

    snprintf(s_topic_cmd, sizeof s_topic_cmd, "%s/%s/commands/measure", topic_prefix(), s_device_id);
    snprintf(s_topic_status, sizeof s_topic_status, "%s/%s/status/measure",   topic_prefix(), s_device_id);

    if (!s_q) s_q = xQueueCreate(4, sizeof(job_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (!s_task) {
        if (xTaskCreate(task_fn, "measure", 4096, NULL, 4, &s_task) != pdPASS)
            return ESP_ERR_NO_MEM;
    }

    // Attach to MQTT if already up
    s_client = telemetry_mqtt_get_client();
    if (s_client) {
        esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    }

    ESP_LOGI(TAG, "measure ready; cmd=%s status=%s (mqtt_attached=%s)", s_topic_cmd, s_topic_status, s_client?"yes":"no");
    return ESP_OK;
}

esp_err_t measure_attach_mqtt(void)
{
    esp_mqtt_client_handle_t c = telemetry_mqtt_get_client();
    if (!c) return ESP_ERR_INVALID_STATE;
    if (s_client != c) s_client = c;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_evt, NULL);
    ESP_LOGI(TAG, "measure attached to MQTT; subscribed on connect to %s", s_topic_cmd);
    return ESP_OK;
}

esp_err_t measure_request(uint8_t sensor, uint32_t num_points, uint32_t *job_id_out)
{
    if (sensor < 1 || sensor > 3) return ESP_ERR_INVALID_ARG;
    if (num_points == 0) num_points = 1;
    if (num_points > CONFIG_APP_MEAS_MAX_POINTS) num_points = CONFIG_APP_MEAS_MAX_POINTS;

    job_t j = { .job_id = ++s_next_job_id, .sensor = sensor, .points = num_points };
    if (xQueueSend(s_q, &j, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    if (job_id_out) *job_id_out = j.job_id;
    publish_status("accepted", &j, NULL, NULL);
    return ESP_OK;
}

bool measure_is_busy(void) { return s_busy; }
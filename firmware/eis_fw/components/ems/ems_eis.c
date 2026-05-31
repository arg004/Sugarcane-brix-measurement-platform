// ====================================================================
// FILE: components/ems/ems_eis.c
// ====================================================================
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "driver/uart.h"          // uart_flush_input()
#include "ems/ems_eis.h"
#include "ems/ems_uart.h"
#include "mux/mux.h"              // 0 = safe/park, 1..3 = sensors
#include "storage/sdcard.h"
#include "telemetry/mqtt_bus.h"
#include "config/config.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "EMS_EIS";

#ifndef CONFIG_EMS_LINE_TIMEOUT_MS
#define CONFIG_EMS_LINE_TIMEOUT_MS 1000
#endif
#ifndef CONFIG_EMS_IDLE_TIMEOUT_MS
#define CONFIG_EMS_IDLE_TIMEOUT_MS 5000
#endif
#ifndef CONFIG_APP_MEAS_DATA_DIR
#define CONFIG_APP_MEAS_DATA_DIR "/sd/data"
#endif

// ---------------------------- MQTT helper ----------------------------
static esp_mqtt_client_handle_t s_client;
static char s_topic_status[128];

// Single-flight gate (ownerless) — THIS fixes your crash
static SemaphoreHandle_t s_sf;   // binary semaphore
static uint32_t s_job_id;

static inline void mqtt_pub_json(cJSON *obj) {
    if (!obj || !s_client) return;
    char *p = cJSON_PrintUnformatted(obj);
    if (p) { esp_mqtt_client_publish(s_client, s_topic_status, p, 0, 1, false); cJSON_free(p); }
}

// ---------------------------- String helpers -------------------------
static inline void trim_crlf(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

// ---------------------- Hard-coded PSTrace EIS -----------------------
static const char *kEIS_MSCR =
    "e\n"
    "var h\n"
    "var r\n"
    "var j\n"
    "var o\n"
    "var d\n"
    "set_pgstat_chan 1\n"
    "set_pgstat_mode 0\n"
    "set_pgstat_chan 0\n"
    "set_pgstat_mode 3\n"
    "set_max_bandwidth 150k\n"
    "set_range_minmax da 0 0\n"
    "cell_off\n"
    "set_range ba 5900u\n"
    "set_autoranging ba 5900u 5900u\n"
    "set_range ab 4200m\n"
    "set_range ba 2950u\n"
    "set_autoranging ba 59n 2950u\n"
    "store_var d 0 ab\n"
    "add_var d o\n"
    "set_e d\n"
    "cell_on\n"
    "meas_loop_eis h r j 5m 150k 10 51 d\n"
    "  pck_start\n"
    "    pck_add h\n"
    "    pck_add r\n"
    "    pck_add j\n"
    "  pck_end\n"
    "endloop\n"
    "on_finished:\n"
    "  cell_off\n";

// Send MethodSCRIPT with LF lines, add empty-line terminator at the end.
static const char *skip_leading_e_or_l(const char *script) {
    if (!script) return NULL;
    const char *nl = strchr(script, '\n');
    if (!nl) return script;
    size_t len = (size_t)(nl - script);
    if (len == 1 && (script[0] == 'e' || script[0] == 'E' || script[0] == 'l' || script[0] == 'L'))
        return nl + 1;
    return script;
}
static esp_err_t ms_send_script_lines(const char *cmd, const char *script) {
    if (!cmd || !script) return ESP_ERR_INVALID_ARG;
    const char *p = skip_leading_e_or_l(script);
    ESP_RETURN_ON_ERROR(ems_send_str(cmd, true, 50), TAG, "cmd");
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len) {
            char line[256];
            if (len >= sizeof line) return ESP_ERR_NO_MEM;
            memcpy(line, p, len);
            line[len] = '\0';
            ESP_RETURN_ON_ERROR(ems_send_str(line, true, 50), TAG, "line");
        }
        p = nl ? nl + 1 : p + len;
    }
    return ems_send_str("", true, 50); // final empty line
}

// ---------------------- Packed P-line parser -------------------------
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}
static double si_from_prefix(char p) {
    switch (p) {
        case 'a': return 1e-18; case 'f': return 1e-15; case 'p': return 1e-12;
        case 'n': return 1e-9;  case 'u': return 1e-6;  case 'm': return 1e-3;
        case 'k': return 1e3;   case 'M': return 1e6;   case 'G': return 1e9;
        default: return 1.0;
    }
}
static bool read_val28(const char *s, size_t *used, double *out) {
    uint32_t x = 0; size_t i = 0; int d, n = 0;
    while ((d = hexv((unsigned char)s[i])) >= 0) { x = (x << 4) | (uint32_t)d; i++; if (++n > 7) break; }
    if (n == 0) return false;
    char suf = s[i++];
    if (suf == 'i') { int32_t si = (int32_t)x - (int32_t)(1u << 27); *out = (double)si; *used = i; return true; }
    int32_t raw = (int32_t)x - (int32_t)(1u << 27);
    *out = ((double)raw) * si_from_prefix(suf); *used = i; return true;
}
typedef struct { double f_hz, z_re, z_im; } eis_row_t;
static bool parse_p_line_eis(const char *line, eis_row_t *row) {
    if (!line || line[0] != 'P') return false;
    memset(row, 0, sizeof *row);
    const char *p = line + 1;
    while (*p) {
        while (*p == ';' || *p == ' ') p++;
        if (*p == '\0' || *p == '\n') break;
        if (!isalpha((unsigned char)p[0]) || !isalpha((unsigned char)p[1])) break;
        char a = (char)tolower((unsigned char)p[0]);
        char b = (char)tolower((unsigned char)p[1]);
        p += 2;
        double v = 0; size_t u = 0;
        if (!read_val28(p, &u, &v)) break;
        p += u;
        if (a == 'c' && b == 'c') row->z_re = v;        // Zreal
        else if (a == 'c' && b == 'd') row->z_im = v;   // Zimag
        else if ((a == 'c' && b == 'g') || (a == 'd' && b == 'c')) row->f_hz = v; // freq
        while (*p && *p != ';' && *p != '\n') {
            if (*p == ',') { size_t u2 = 0; double tmp; if (!read_val28(p + 1, &u2, &tmp)) break; p += 1 + u2; }
            else p++;
        }
    }
    return true;
}

// ---------------------------- CSV helpers ---------------------------
static void stamp_now(char *buf, size_t cap) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    strftime(buf, cap, "%Y%m%dT%H%M%S", &tm);
}
static void make_path_long(char *out, size_t cap, uint8_t sensor, uint32_t job) {
    char dev[64] = {0};
    if (config_get_device_id(dev, sizeof dev) != ESP_OK || !dev[0]) snprintf(dev, sizeof dev, "unknown");
    char ts[32]; stamp_now(ts, sizeof ts);
    snprintf(out, cap, "%s/%s_S%u_%s_%06u.csv", CONFIG_APP_MEAS_DATA_DIR, dev, (unsigned)sensor, ts, (unsigned)job);
}
static void make_path_83(char *out, size_t cap, uint8_t sensor, uint32_t job) {
    unsigned n = job % 100000u;
    snprintf(out, cap, "%s/S%u%05u.CSV", CONFIG_APP_MEAS_DATA_DIR, (unsigned)sensor, n);
}

// --------------------------- Runner task ----------------------------
typedef struct { uint8_t sensor; } eis_cfg_t;

static void eis_task(void *arg) {
    eis_cfg_t cfg = *(eis_cfg_t *)arg; free(arg);
    uint32_t job = ++s_job_id;

    cJSON *start = cJSON_CreateObject();
    cJSON_AddStringToObject(start, "op", "eis_start");
    cJSON_AddNumberToObject(start, "sensor", cfg.sensor);
    cJSON_AddNumberToObject(start, "job", (double)job);
    mqtt_pub_json(start); cJSON_Delete(start);

    if (!sdcard_is_mounted()) { cJSON *e=cJSON_CreateObject(); cJSON_AddStringToObject(e,"op","eis_error"); cJSON_AddStringToObject(e,"err","sd_not_mounted"); mqtt_pub_json(e); xSemaphoreGive(s_sf); vTaskDelete(NULL); return; }
    if (cfg.sensor < 1 || cfg.sensor > 3) { cJSON *e=cJSON_CreateObject(); cJSON_AddStringToObject(e,"op","eis_error"); cJSON_AddStringToObject(e,"err","bad_sensor"); mqtt_pub_json(e); xSemaphoreGive(s_sf); vTaskDelete(NULL); return; }

    if (mux_select(cfg.sensor) != ESP_OK) { cJSON *e=cJSON_CreateObject(); cJSON_AddStringToObject(e,"op","eis_error"); cJSON_AddStringToObject(e,"err","mux_select_failed"); mqtt_pub_json(e); xSemaphoreGive(s_sf); vTaskDelete(NULL); return; }
    vTaskDelay(pdMS_TO_TICKS(50)); // settle

    char path[256]; FILE *csv = NULL;
    make_path_long(path, sizeof path, cfg.sensor, job);
    csv = fopen(path, "w");
    if (!csv) {
        char p83[128]; make_path_83(p83, sizeof p83, cfg.sensor, job);
        csv = fopen(p83, "w"); if (csv) strncpy(path, p83, sizeof path);
    }
    if (!csv) { cJSON *e=cJSON_CreateObject(); cJSON_AddStringToObject(e,"op","eis_error"); cJSON_AddStringToObject(e,"err","csv_open_fail"); mqtt_pub_json(e); mux_select(0); xSemaphoreGive(s_sf); vTaskDelete(NULL); return; }
    fprintf(csv, "freq, Z', Z''\n");

    char line[512];
    const int line_to = CONFIG_EMS_LINE_TIMEOUT_MS;
    const int idle_to = CONFIG_EMS_IDLE_TIMEOUT_MS;
    int idle_left = idle_to;
    bool echo = false, error = false;
    size_t rows = 0;

    ems_io_lock();
    uart_flush_input(CONFIG_APP_EMS_UART_NUM);
    esp_err_t wr = ms_send_script_lines("e", kEIS_MSCR);

    while (wr == ESP_OK && idle_left >= 0) {
        int rd = ems_read_until(line, sizeof line, "\n", line_to);
        if (rd < 0) { idle_left -= line_to; continue; }
        idle_left = idle_to; trim_crlf(line);
        if (!echo && line[0] == 'e') { memmove(line, line + 1, strlen(line)); echo = true; if (line[0] == '\0') continue; }
        if (line[0] == '!') { error = true; break; }
        if (line[0] == '\0') { break; }
        if (line[0] == 'M' || line[0] == '*') { continue; }
        if (line[0] == 'P') {
            eis_row_t r; if (parse_p_line_eis(line, &r)) {
                fprintf(csv, "%.9g, %.9g, %.9g\n", r.f_hz, r.z_re, r.z_im);
                rows++;
                if ((rows & 0x3F) == 0) {
                    fflush(csv);
                    cJSON *p = cJSON_CreateObject();
                    cJSON_AddStringToObject(p, "op", "eis_progress");
                    cJSON_AddNumberToObject(p, "rows", (double)rows);
                    mqtt_pub_json(p); cJSON_Delete(p);
                }
            }
        }
    }

    ems_io_unlock();
    fflush(csv); fclose(csv);
    mux_select(0);  // park

    if (wr != ESP_OK) { cJSON *e=cJSON_CreateObject(); cJSON_AddStringToObject(e,"op","eis_error"); cJSON_AddStringToObject(e,"err","write_fail");    mqtt_pub_json(e); xSemaphoreGive(s_sf); vTaskDelete(NULL); return; }
    if (error)       { cJSON *e=cJSON_CreateObject(); cJSON_AddStringToObject(e,"op","eis_error"); cJSON_AddStringToObject(e,"err","ms_error");      cJSON_AddStringToObject(e,"last", line); mqtt_pub_json(e); xSemaphoreGive(s_sf); vTaskDelete(NULL); return; }
    if (idle_left<0) { cJSON *e=cJSON_CreateObject(); cJSON_AddStringToObject(e,"op","eis_error"); cJSON_AddStringToObject(e,"err","idle_timeout");  mqtt_pub_json(e); xSemaphoreGive(s_sf); vTaskDelete(NULL); return; }

    cJSON *done = cJSON_CreateObject();
    cJSON_AddStringToObject(done, "op", "eis_done");
    cJSON_AddBoolToObject(done, "ok", true);
    cJSON_AddStringToObject(done, "csv", path);
    cJSON_AddNumberToObject(done, "rows", (double)rows);
    mqtt_pub_json(done); cJSON_Delete(done);

    xSemaphoreGive(s_sf);
    vTaskDelete(NULL);
}

// ----------------------------- Public API ---------------------------
esp_err_t ems_eis_init(void) {
    static bool inited = false;
    if (!inited) {
        s_sf = xSemaphoreCreateBinary();
        if (!s_sf) return ESP_ERR_NO_MEM;
        // Make it "available" initially
        if (uxSemaphoreGetCount(s_sf) == 0) xSemaphoreGive(s_sf);
        inited = true;
    }

    char id[64] = {0};
    if (config_get_device_id(id, sizeof id) != ESP_OK || id[0] == '\0') snprintf(id, sizeof id, "unknown");
    snprintf(s_topic_status, sizeof s_topic_status, "devices/%s/status/ems", id);
    (void)telemetry_mqtt_ensure_started();
    s_client = telemetry_mqtt_get_client();
    return ESP_OK;
}

esp_err_t ems_eis_handle_mqtt(cJSON *root) {
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    if (!cJSON_IsString(op) || !op->valuestring) return ESP_ERR_NOT_FOUND;
    if (!(strcmp(op->valuestring, "eis.run") == 0 || strcmp(op->valuestring, "eis") == 0))
        return ESP_ERR_NOT_FOUND;

    // Single-flight: non-blocking take; if busy, report immediately
    if (xSemaphoreTake(s_sf, 0) != pdTRUE) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "op", "eis_error");
        cJSON_AddStringToObject(j, "err", "busy");
        mqtt_pub_json(j); cJSON_Delete(j);
        return ESP_ERR_INVALID_STATE;
    }

    eis_cfg_t *heap = malloc(sizeof *heap);
    if (!heap) { xSemaphoreGive(s_sf); return ESP_ERR_NO_MEM; }
    heap->sensor = 1; // default
    const cJSON *sensor = cJSON_GetObjectItemCaseSensitive(root, "sensor");
    if (cJSON_IsNumber(sensor)) heap->sensor = (uint8_t)sensor->valuedouble;

    if (xTaskCreatePinnedToCore(eis_task, "eis_job", 8192, heap, 5, NULL, tskNO_AFFINITY) != pdPASS) {
        free(heap); xSemaphoreGive(s_sf); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
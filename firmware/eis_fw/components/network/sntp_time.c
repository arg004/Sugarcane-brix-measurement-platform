// firmware/components/network/sntp_time.c  (new)
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network/sntp_time.h"

static const char *TAG = "SNTP";

void sntp_start(const char *tz) {
    // Set timezone before sync so localtime is correct immediately
    const char *tz_in = (tz && tz[0]) ? tz : "CST6CDT,M3.2.0/2,M11.1.0/2"; // US Central
    setenv("TZ", tz_in, 1);
    tzset();

    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP started (tz=%s)", tz_in);
    }
}

static bool has_time(void) {
    time_t now = 0; struct tm tm = {0};
    time(&now);
    localtime_r(&now, &tm);
    return (tm.tm_year + 1900) > 2020; // crude check
}

bool sntp_wait_for_sync(unsigned timeout_ms) {
    const unsigned step = 200;
    unsigned waited = 0;
    while (waited < timeout_ms) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED || has_time()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    return has_time();
}

void sntp_format_time_now(char *out, size_t len) {
    if (!out || len < 2) return;
    time_t now = 0; struct tm tm = {0};
    time(&now);
    localtime_r(&now, &tm);
    strftime(out, len, "%Y-%m-%dT%H:%M:%S%z", &tm);
}
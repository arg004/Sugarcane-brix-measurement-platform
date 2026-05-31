// SD card detect + auto mount/unmount
// - Uses CONFIG_APP_SD_CD_GPIO (default 41)
// - Active level configurable (default: active-low)
// - Debounced via a tiny task; ISR posts edges to a queue
// - Calls existing sdcard_mount()/sdcard_unmount() from sd_storage.c

#include <stdbool.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "storage/sdcard.h"  // our public API (sdcard_mount/unmount/is_mounted)

#ifndef CONFIG_APP_SD_CD_GPIO
#define CONFIG_APP_SD_CD_GPIO 41
#endif
#ifndef CONFIG_APP_SD_CD_ACTIVE_LOW
#define CONFIG_APP_SD_CD_ACTIVE_LOW 1   // 1 → low = inserted
#endif
#ifndef CONFIG_APP_SD_CD_DEBOUNCE_MS
#define CONFIG_APP_SD_CD_DEBOUNCE_MS 80
#endif
#ifndef CONFIG_APP_SD_AUTO_MOUNT
#define CONFIG_APP_SD_AUTO_MOUNT 1
#endif

static const char *TAG = "SDCD";

static int s_cd_gpio = CONFIG_APP_SD_CD_GPIO;
static bool s_active_low = (CONFIG_APP_SD_CD_ACTIVE_LOW != 0);
static QueueHandle_t s_cd_q;
static TaskHandle_t s_cd_task;
static bool s_started = false;

static inline bool decode_inserted_level(int level)
{
    // If active-low, level==0 means inserted; otherwise level==1 means inserted
    return s_active_low ? (level == 0) : (level != 0);
}

bool sdcard_is_inserted(void)
{
    int lvl = gpio_get_level(s_cd_gpio);
    return decode_inserted_level(lvl);
}

static void IRAM_ATTR cd_isr(void *arg)
{
    (void)arg;
    if (!s_cd_q) return;
    int lvl = gpio_get_level(s_cd_gpio);
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_cd_q, &lvl, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static void cd_task(void *arg)
{
    (void)arg;
    const TickType_t debounce = pdMS_TO_TICKS(CONFIG_APP_SD_CD_DEBOUNCE_MS);

    // Read initial state and act once at startup
    bool inserted = sdcard_is_inserted();
    ESP_LOGI(TAG, "CD init: gpio=%d active_%s, inserted=%s", s_cd_gpio,
             s_active_low ? "low" : "high", inserted ? "yes" : "no");
#if CONFIG_APP_SD_AUTO_MOUNT
    if (inserted && !sdcard_is_mounted()) {
        esp_err_t r = sdcard_mount(NULL);
        ESP_LOGI(TAG, "auto-mount on boot: %s", (r == ESP_OK) ? "OK" : esp_err_to_name(r));
    }
#endif

    // Debounced edge handling loop
    for (;;) {
        int lvl;
        if (xQueueReceive(s_cd_q, &lvl, portMAX_DELAY) != pdTRUE) continue;
        vTaskDelay(debounce);
        // Re-check after debounce
        bool now = sdcard_is_inserted();
        if (now == inserted) continue;  // bounce
        inserted = now;

        if (now) {
#if CONFIG_APP_SD_AUTO_MOUNT
            ESP_LOGI(TAG, "card inserted → mount");
            esp_err_t r = sdcard_mount(NULL);
            if (r != ESP_OK) {
                ESP_LOGW(TAG, "mount failed after insert: %s", esp_err_to_name(r));
            }
#else
            ESP_LOGI(TAG, "card inserted (auto-mount disabled)");
#endif
        } else {
            ESP_LOGI(TAG, "card removed → unmount");
            sdcard_unmount();
        }
    }
}

esp_err_t sdcard_cd_start(void)
{
    if (s_started) return ESP_OK;

    // Basic validation
    if (s_cd_gpio < 0 || s_cd_gpio > 48) return ESP_ERR_INVALID_ARG;

    // Configure CD GPIO as input with a safe default pull
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_cd_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = s_active_low ? 1 : 0,   // typical sockets short to GND when inserted
        .pull_down_en = s_active_low ? 0 : 1,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t r = gpio_config(&io);
    if (r != ESP_OK) return r;

    // ISR service (idempotent install)
    r = gpio_install_isr_service(0);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r; // INVALID_STATE means already installed

    r = gpio_isr_handler_add(s_cd_gpio, cd_isr, NULL);
    if (r != ESP_OK) return r;

    if (!s_cd_q) s_cd_q = xQueueCreate(8, sizeof(int));
    if (!s_cd_q) return ESP_ERR_NO_MEM;

    if (xTaskCreate(cd_task, "sd_cd", 3072, NULL, 4, &s_cd_task) != pdPASS) return ESP_ERR_NO_MEM;

    s_started = true;
    return ESP_OK;
}

void sdcard_cd_stop(void)
{
    if (!s_started) return;
    gpio_isr_handler_remove(s_cd_gpio);
    if (s_cd_task) { vTaskDelete(s_cd_task); s_cd_task = NULL; }
    if (s_cd_q) { vQueueDelete(s_cd_q); s_cd_q = NULL; }
    s_started = false;
}
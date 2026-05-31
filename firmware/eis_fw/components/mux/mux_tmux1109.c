// =============================================================
// FILE: components/mux/mux_tmux1109.c
// =============================================================
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "mux/mux.h"

#ifndef ESP_RETURN_ON_ERROR
#define ESP_RETURN_ON_ERROR(x, tag, msg) do { \
    esp_err_t __e = (x); \
    if (__e != ESP_OK) { ESP_LOGE(tag, "%s: %s", msg, esp_err_to_name(__e)); return __e; } \
} while (0)
#endif

static const char *TAG = "MUX";

// ---- GPIO defaults (override via Kconfig if available) ----
#ifndef CONFIG_APP_MUX1_A0_PIN
#define CONFIG_APP_MUX1_A0_PIN 6   // CE/RE A0
#endif
#ifndef CONFIG_APP_MUX1_A1_PIN
#define CONFIG_APP_MUX1_A1_PIN 7   // CE/RE A1
#endif
#ifndef CONFIG_APP_MUX2_A0_PIN
#define CONFIG_APP_MUX2_A0_PIN 4   // WE A0
#endif
#ifndef CONFIG_APP_MUX2_A1_PIN
#define CONFIG_APP_MUX2_A1_PIN 5   // WE A1
#endif
#ifndef CONFIG_APP_MUX_SAFE_SEL
#define CONFIG_APP_MUX_SAFE_SEL 3  // 11b = SAFE
#endif

static int s_a0_1 = CONFIG_APP_MUX1_A0_PIN;  // CE/RE A0
static int s_a1_1 = CONFIG_APP_MUX1_A1_PIN;  // CE/RE A1
static int s_a0_2 = CONFIG_APP_MUX2_A0_PIN;  // WE A0
static int s_a1_2 = CONFIG_APP_MUX2_A1_PIN;  // WE A1
static bool s_inited = false;

static esp_err_t prep_all(void)
{
    if (s_inited) return ESP_OK;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<s_a0_1) | (1ULL<<s_a1_1) | (1ULL<<s_a0_2) | (1ULL<<s_a1_2),
        .mode = GPIO_MODE_INPUT_OUTPUT,   // **enable input path** so gpio_get_level reflects latch
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "cfg");
    s_inited = true;
    return ESP_OK;
}

static esp_err_t apply_code(uint8_t code)
{
    ESP_RETURN_ON_ERROR(prep_all(), TAG, "prep");

    const int b0 = (code >> 0) & 1;  // A0
    const int b1 = (code >> 1) & 1;  // A1

    // Drive both TMUX1109 select pairs identically
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)s_a0_1, b0), TAG, "a0_1");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)s_a1_1, b1), TAG, "a1_1");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)s_a0_2, b0), TAG, "a0_2");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)s_a1_2, b1), TAG, "a1_2");

    // Readback (now valid because input path is enabled)
    const int r11 = gpio_get_level((gpio_num_t)s_a1_1);
    const int r10 = gpio_get_level((gpio_num_t)s_a0_1);
    const int r21 = gpio_get_level((gpio_num_t)s_a1_2);
    const int r20 = gpio_get_level((gpio_num_t)s_a0_2);
    ESP_LOGI(TAG, "code=%u → CE/RE(A1:A0)=%d:%d  WE(A1:A0)=%d:%d", code, r11, r10, r21, r20);
    return ESP_OK;
}

esp_err_t mux_init(void)
{
    ESP_RETURN_ON_ERROR(prep_all(), TAG, "prep");
    ESP_RETURN_ON_ERROR(apply_code(CONFIG_APP_MUX_SAFE_SEL), TAG, "safe");
    ESP_LOGI(TAG, "MUX ready A0/A1: CE/RE=(%d,%d) WE=(%d,%d) safe=%u",
             s_a0_1, s_a1_1, s_a0_2, s_a1_2, CONFIG_APP_MUX_SAFE_SEL);
    return ESP_OK;
}

esp_err_t mux_select(uint8_t sensor)
{
    uint8_t code;
    switch (sensor) {
        case 0: code = CONFIG_APP_MUX_SAFE_SEL; break; // SAFE (11)
        case 1: code = 0; break;  // 00
        case 2: code = 1; break;  // 01
        case 3: code = 2; break;  // 10
        default: return ESP_ERR_INVALID_ARG;
    }
    return apply_code(code);
}
#include "mux/mux.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"

#ifndef CONFIG_APP_MUX_CERE_A0
#define CONFIG_APP_MUX_CERE_A0 6
#endif
#ifndef CONFIG_APP_MUX_CERE_A1
#define CONFIG_APP_MUX_CERE_A1 7
#endif
#ifndef CONFIG_APP_MUX_WE_A0
#define CONFIG_APP_MUX_WE_A0 4
#endif
#ifndef CONFIG_APP_MUX_WE_A1
#define CONFIG_APP_MUX_WE_A1 5
#endif
#ifndef CONFIG_APP_MUX_PARK_CHANNEL
#define CONFIG_APP_MUX_PARK_CHANNEL 0
#endif

static const char *TAG = "MUX";

static int s_pins[4] = {
    CONFIG_APP_MUX_CERE_A0,
    CONFIG_APP_MUX_CERE_A1,
    CONFIG_APP_MUX_WE_A0,
    CONFIG_APP_MUX_WE_A1,
};
static uint8_t s_sel = CONFIG_APP_MUX_PARK_CHANNEL;

static inline void set_pair(int a0, int a1, uint8_t sel2)
{
    // sel2 is 0..3 => A0 = LSB, A1 = MSB
    gpio_set_level(a0, (sel2 & 0x1) ? 1 : 0);
    gpio_set_level(a1, (sel2 & 0x2) ? 1 : 0);
}

static esp_err_t cfg_out(int io)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << io),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_conf);
}

esp_err_t mux_init(void)
{
    for (int i = 0; i < 4; ++i) {
        if (s_pins[i] < 0 || s_pins[i] > 48) {
            ESP_LOGE(TAG, "invalid GPIO %d", s_pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
        ESP_ERROR_CHECK(cfg_out(s_pins[i]));
    }
    // Park on channel 0 (or your configured park channel)
    set_pair(s_pins[0], s_pins[1], CONFIG_APP_MUX_PARK_CHANNEL);
    set_pair(s_pins[2], s_pins[3], CONFIG_APP_MUX_PARK_CHANNEL);
    s_sel = CONFIG_APP_MUX_PARK_CHANNEL;
    ESP_LOGI(TAG, "ready: CERE(A0=%d,A1=%d) WE(A0=%d,A1=%d) → park=%u",
             s_pins[0], s_pins[1], s_pins[2], s_pins[3], s_sel);
    return ESP_OK;
}

esp_err_t mux_select(uint8_t sensor)
{
    if (sensor > 3) return ESP_ERR_INVALID_ARG;
    uint8_t ch = sensor;      // 0=park, 1..3 = your sensors
    set_pair(s_pins[0], s_pins[1], ch);
    set_pair(s_pins[2], s_pins[3], ch);
    s_sel = ch;
    return ESP_OK;
}

esp_err_t mux_park(void)
{
    return mux_select(CONFIG_APP_MUX_PARK_CHANNEL);
}

uint8_t mux_current(void)
{
    return s_sel;
}

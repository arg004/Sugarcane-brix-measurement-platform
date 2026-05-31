// FILE: main/mux_blink_hardcoded.c
// Purpose: drive the four MUX select pins directly, 1 Hz, print readbacks.
// No tasks, no MQTT, no component deps. Drop-in and call mux_blink_hardcoded() from app_main.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

// --- Edit here if your board uses different GPIOs ---
#ifndef PIN_CE_A0
#define PIN_CE_A0  (gpio_num_t)6
#endif
#ifndef PIN_CE_A1
#define PIN_CE_A1  (gpio_num_t)7
#endif
#ifndef PIN_WE_A0
#define PIN_WE_A0  (gpio_num_t)4
#endif
#ifndef PIN_WE_A1
#define PIN_WE_A1  (gpio_num_t)5
#endif

static const char *TAG = "MUX_BLINK_HC";

static void cfg_input_pullup(gpio_num_t io)
{
    gpio_reset_pin(io);
    gpio_set_direction(io, GPIO_MODE_INPUT);
    gpio_pullup_en(io);
    gpio_pulldown_dis(io);
}

static void cfg_output(gpio_num_t io)
{
    gpio_reset_pin(io);
    gpio_set_direction(io, GPIO_MODE_INPUT_OUTPUT);
    gpio_pullup_dis(io);
    gpio_pulldown_dis(io);
}

void mux_test(void)
{
    const gpio_num_t pins[4] = { PIN_CE_A0, PIN_CE_A1, PIN_WE_A0, PIN_WE_A1 };
    const char *names[4]     = { "CE_A0",   "CE_A1",   "WE_A0",   "WE_A1"   };

    ESP_LOGI(TAG, "blink pins CE(A0,A1)=(%d,%d) WE(A0,A1)=(%d,%d)",
             (int)PIN_CE_A0, (int)PIN_CE_A1, (int)PIN_WE_A0, (int)PIN_WE_A1);

    // First pass: sanity check each pin as input+pullup, then try LOW/HIGH
    for (int i = 0; i < 4; ++i) {
        int input_pullup = 0, r_low = 0, r_high = 0;

        cfg_input_pullup(pins[i]);
        vTaskDelay(pdMS_TO_TICKS(20));
        input_pullup = gpio_get_level(pins[i]);

        cfg_output(pins[i]);
        gpio_set_level(pins[i], 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        r_low = gpio_get_level(pins[i]);

        gpio_set_level(pins[i], 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        r_high = gpio_get_level(pins[i]);

        ESP_LOGI(TAG, "sanity %s: input_pullup=%d  low->%d  high->%d",
                 names[i], input_pullup, r_low, r_high);
    }

    // Second pass: blink sequence for scope/LED check
    ESP_LOGI(TAG, "blink sequence start (4 pins × 4 cycles @1s)");
    for (int cycle = 0; cycle < 4; ++cycle) {
        for (int i = 0; i < 4; ++i) {
            cfg_output(pins[i]);
            gpio_set_level(pins[i], 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            int rb = gpio_get_level(pins[i]);
            ESP_LOGI(TAG, "step %02d: %s=1  readback=%d", (cycle * 4) + i, names[i], rb);
            gpio_set_level(pins[i], 0);
        }
    }
    ESP_LOGI(TAG, "blink sequence done");
}
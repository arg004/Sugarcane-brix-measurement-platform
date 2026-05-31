// ==============================
// FILE: components/ems/ems_uart.c
// ==============================
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_check.h"
#include "ems/ems_uart.h"

static const char *TAG = "EMS_UART";

#ifndef CONFIG_APP_EMS_UART_NUM
#define CONFIG_APP_EMS_UART_NUM UART_NUM_1
#endif
#ifndef CONFIG_APP_EMS_TX
#define CONFIG_APP_EMS_TX 17
#endif
#ifndef CONFIG_APP_EMS_RX
#define CONFIG_APP_EMS_RX 18
#endif
#ifndef CONFIG_APP_EMS_BAUD
#define CONFIG_APP_EMS_BAUD 230400
#endif

static SemaphoreHandle_t s_lock;
static inline TickType_t ms_to_ticks(int ms){ return (ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(ms); }

void ems_io_lock(void){ if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void ems_io_unlock(void){ if (s_lock) xSemaphoreGive(s_lock); }

esp_err_t ems_uart_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    const uart_config_t cfg = (uart_config_t){
        .baud_rate = CONFIG_APP_EMS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if ESP_IDF_VERSION_MAJOR >= 5
        .source_clk = UART_SCLK_DEFAULT,
#endif
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(CONFIG_APP_EMS_UART_NUM, 4096, 0, 0, NULL, 0), TAG, "install");
    ESP_RETURN_ON_ERROR(uart_param_config(CONFIG_APP_EMS_UART_NUM, &cfg), TAG, "param");
    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_APP_EMS_UART_NUM, CONFIG_APP_EMS_TX, CONFIG_APP_EMS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "pins");
    ESP_LOGI(TAG, "UART%d ready: TX=%d RX=%d @%d", CONFIG_APP_EMS_UART_NUM, CONFIG_APP_EMS_TX, CONFIG_APP_EMS_RX, CONFIG_APP_EMS_BAUD);
    return ESP_OK;
}

static bool is_delim(unsigned char c, const char *delims){ if(!delims) return false; for(const char *p=delims; *p; ++p) if((unsigned char)*p==c) return true; return false; }

int ems_read_until(char *out, size_t out_cap, const char *delims, int timeout_ms)
{
    if (!out || out_cap==0) return -1;
    size_t w = 0;
    TickType_t deadline = xTaskGetTickCount() + ms_to_ticks(timeout_ms);
    for (;;) {
        uint8_t ch; int rd = uart_read_bytes(CONFIG_APP_EMS_UART_NUM, &ch, 1, 10 / portTICK_PERIOD_MS);
        if (rd == 1) {
            if (w + 1 < out_cap) out[w++] = (char)ch; // keep room for NUL
            if (is_delim(ch, delims)) break;
        } else {
            if ((int)(deadline - xTaskGetTickCount()) <= 0) break;
        }
    }
    out[w] = '\0';
    return (w == 0) ? -1 : (int)w;
}

static size_t rstrip_crlf(char *s){ if(!s) return 0; size_t n=strlen(s); while(n && (s[n-1]=='\n'||s[n-1]=='\r')) s[--n]='\0'; return n; }

esp_err_t ems_probe(char *version_out, size_t out_sz, int timeout_ms)
{
    if (!version_out || out_sz < 2) return ESP_ERR_INVALID_ARG;
    ems_io_lock();
    uart_flush_input(CONFIG_APP_EMS_UART_NUM);
    esp_err_t wr = ems_send_str("t", true, 50);
    int rd = -1; if (wr==ESP_OK) rd = ems_read_until(version_out, out_sz, "\n", timeout_ms);
    ems_io_unlock();
    if (wr != ESP_OK || rd < 0) return ESP_ERR_TIMEOUT;
    rstrip_crlf(version_out);
    return ESP_OK;
}

esp_err_t ems_send_str(const char *s, bool append_lf, int tx_timeout_ms)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    size_t n = strlen(s);
    if (n) {
        int wr = uart_write_bytes(CONFIG_APP_EMS_UART_NUM, s, n);
        if (wr < 0 || (size_t)wr != n) return ESP_FAIL;
    }
    if (append_lf) {
        const char lf='\n'; if (uart_write_bytes(CONFIG_APP_EMS_UART_NUM, &lf, 1) != 1) return ESP_FAIL;
    }
    (void)uart_wait_tx_done(CONFIG_APP_EMS_UART_NUM, ms_to_ticks(tx_timeout_ms));
    return ESP_OK;
}

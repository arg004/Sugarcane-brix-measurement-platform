// firmware/main/app_main.c  (replace the Step 4 usage with event waits)
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "config/config.h"
#include "network/network_task.h"
#include "telemetry/heartbeat.h"
#include "regen/regen_ctrl.h"
#include "storage/sdcard.h"
#include "measure/measure.h"
#include "ems/ems_uart.h"
#include "mux/mux.h"

static const char *TAG = "APP";

esp_err_t ems_cmd_init(void); // forward from ems_cmd.c

extern void mux_test(void);

static void init_nvs_or_erase(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

void app_main(void) {

    // 1) Immediately drive regen pins to safe HIGH at boot
    regen_gpio_safe_boot_init();

    init_nvs_or_erase();

    ESP_ERROR_CHECK(config_init());

    char device_id[64] = {0};
    ESP_ERROR_CHECK(config_get_device_id(device_id, sizeof(device_id)));
    ESP_LOGI(TAG, "DeviceID=%s", device_id);

    // why: start background orchestrator; other modules will wait on its bits
    network_task_start();

    // what: wait up to 15s for IP (non-fatal if timeouts); consumers can choose their own timeouts
    xEventGroupWaitBits(network_events(), NET_EVT_WIFI_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    // what: wait up to another 15s for time sync; continue even if not synced yet
    xEventGroupWaitBits(network_events(), NET_EVT_TIME_SYNCED, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    // why: background periodic heartbeats, transport-agnostic
    heartbeat_set_extra_kv("role", "node");
    heartbeat_start();

    ESP_ERROR_CHECK(regen_init());
    ESP_ERROR_CHECK(regen_attach_mqtt());


    ESP_ERROR_CHECK(measure_init());
    ESP_ERROR_CHECK(measure_attach_mqtt());

    ESP_ERROR_CHECK(sdcard_cd_start());

    ems_cmd_init();

    // mux_test();

    // mux_init();
    // mux_select(0); vTaskDelay(pdMS_TO_TICKS(5000));
    // mux_select(1); vTaskDelay(pdMS_TO_TICKS(5000));
    // mux_select(2); vTaskDelay(pdMS_TO_TICKS(5000));
    // mux_select(3); vTaskDelay(pdMS_TO_TICKS(5000));
    // mux_select(0); // SAFE


    // optional: let the app idle; future modules will start here
}
// firmware/components/config/nvs_config.c (new)
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"     // esp_efuse_mac_get_default
#include "esp_system.h"
#include "esp_random.h"
#include "config/config.h"

static const char *TAG = "CFG";
static const char *NVS_NS = "cfg";
static const char *KEY_DEVICE_ID = "device_id";

static esp_err_t open_ns(nvs_handle_t *out) {
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) *out = h;
    return err;
}

static void make_device_id_from_mac(char *buf, size_t len) {
    // Why: stable ID without RNG/time; debuggable.
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(buf, len, "ESP32S3-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        uint32_t r = esp_random();
        snprintf(buf, len, "ESP32S3-%08" PRIX32, r);
    }
}

esp_err_t config_init(void) {
    nvs_handle_t h = 0;
    esp_err_t err = open_ns(&h);
    if (err == ESP_OK) nvs_close(h);
    return err;
}

esp_err_t config_get_device_id(char *out, size_t out_len) {
    if (!out || out_len < 8) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h = 0;
    esp_err_t err = open_ns(&h);
    if (err != ESP_OK) return err;

    size_t need = out_len;
    err = nvs_get_str(h, KEY_DEVICE_ID, out, &need);
    if (err == ESP_OK) { nvs_close(h); return ESP_OK; }
    if (err != ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return err; }

    make_device_id_from_mac(out, out_len);
    ESP_LOGI(TAG, "Creating DeviceID: %s", out);

    esp_err_t w = nvs_set_str(h, KEY_DEVICE_ID, out);
    if (w == ESP_OK) w = nvs_commit(h);
    nvs_close(h);
    return w;
}

esp_err_t config_set_str(const char *key, const char *val) {
    if (!key || !val) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h = 0;
    esp_err_t err = open_ns(&h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_get_str(const char *key, char *out, size_t out_len) {
    if (!key || !out || out_len == 0) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h = 0;
    esp_err_t err = open_ns(&h);
    if (err != ESP_OK) return err;
    size_t need = out_len;
    err = nvs_get_str(h, key, out, &need);
    nvs_close(h);
    return err;
}
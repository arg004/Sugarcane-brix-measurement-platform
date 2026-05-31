#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "driver/spi_master.h"
#include "driver/sdspi_host.h"

#include "sdkconfig.h"

static const char *TAG = "SDSTORE";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static bool s_used_spi = false;
static spi_host_device_t s_spi_host = SPI2_HOST; // default; set from Kconfig on use

#ifndef CONFIG_APP_SD_MOUNT_POINT
#define CONFIG_APP_SD_MOUNT_POINT "/sdcard"
#endif

static inline const char* mp(void) { return CONFIG_APP_SD_MOUNT_POINT; }

static void enable_pullups_for_sd_lines(void) {
#if CONFIG_APP_SD_INTERNAL_PULLUPS
    // Pull-ups on CMD and DAT[0..3]; never pull-up CLK
    const int pins[] = {
        CONFIG_APP_SDMMC_CMD,
        CONFIG_APP_SDMMC_D0,
        CONFIG_APP_SDMMC_D1,
        CONFIG_APP_SDMMC_D2,
        CONFIG_APP_SDMMC_D3
    };
    for (size_t i = 0; i < sizeof(pins)/sizeof(pins[0]); ++i) {
        if (pins[i] >= 0) gpio_set_pull_mode((gpio_num_t)pins[i], GPIO_PULLUP_ONLY);
    }
#endif
}

static void pullups_for_spi(void) {
#if CONFIG_APP_SD_INTERNAL_PULLUPS
    // For SPI, pull-up MOSI (CMD), MISO (DAT0), CS (DAT3). CLK floats.
    const int pins[] = { CONFIG_APP_SDMMC_CMD, CONFIG_APP_SDMMC_D0, CONFIG_APP_SDMMC_D3 };
    for (size_t i = 0; i < sizeof(pins)/sizeof(pins[0]); ++i) {
        if (pins[i] >= 0) gpio_set_pull_mode((gpio_num_t)pins[i], GPIO_PULLUP_ONLY);
    }
#endif
}

static esp_vfs_fat_sdmmc_mount_config_t make_mount_cfg(void) {
    esp_vfs_fat_sdmmc_mount_config_t m = {
        .format_if_mount_failed = false,
        .max_files = CONFIG_APP_SD_MAX_FILES,
        .allocation_unit_size = 16 * 1024,
    };
    return m;
}

/* ---------- SDMMC path (4-bit or 1-bit) ---------- */
static esp_err_t try_mount_sdmmc(void) {
    enable_pullups_for_sd_lines();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; // conservative + stable

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = CONFIG_APP_SDMMC_CLK;
    slot.cmd = CONFIG_APP_SDMMC_CMD;
    slot.d0  = CONFIG_APP_SDMMC_D0;
    slot.d1  = CONFIG_APP_SDMMC_D1;
    slot.d2  = CONFIG_APP_SDMMC_D2;
    slot.d3  = CONFIG_APP_SDMMC_D3;
    slot.width = CONFIG_APP_SDMMC_WIDTH_4BIT ? 4 : 1;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = make_mount_cfg();

    esp_err_t err = esp_vfs_fat_sdmmc_mount(mp(), &host, &slot, &mount_cfg, &s_card);
    if (err == ESP_OK) {
        s_used_spi = false;
        s_mounted = true;
        sdmmc_card_print_info(stdout, s_card);
        ESP_LOGI(TAG, "SDMMC mounted (%s, width=%d)", mp(), (int)slot.width);
        return ESP_OK;
    }

    // Clean up host if the mount failed (avoid lingering ISR/task)
    if (host.deinit) host.deinit();

    ESP_LOGW(TAG, "SDMMC mount failed: %s (will try SPI)", esp_err_to_name(err));
    return err;
}

/* ---------- SPI fallback (SDSPI) ---------- */
static esp_err_t try_mount_sdspi(void) {
    pullups_for_spi();

    s_spi_host = (spi_host_device_t)((CONFIG_APP_SD_SPI_HOST == 3) ? SPI3_HOST : SPI2_HOST);

    // CMD->MOSI, DAT0->MISO, CLK->CLK, DAT3->CS
    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_APP_SDMMC_CMD,
        .miso_io_num = CONFIG_APP_SDMMC_D0,
        .sclk_io_num = CONFIG_APP_SDMMC_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
        .flags = 0
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(s_spi_host, &buscfg, SPI_DMA_CH_AUTO));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_spi_host;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = CONFIG_APP_SDMMC_D3;   // DAT3 as CS
    slot.host_id = s_spi_host;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = make_mount_cfg();

    esp_err_t err = esp_vfs_fat_sdspi_mount(mp(), &host, &slot, &mount_cfg, &s_card);
    if (err == ESP_OK) {
        s_used_spi = true;
        s_mounted = true;
        sdmmc_card_print_info(stdout, s_card);
        ESP_LOGI(TAG, "SDSPI mounted (%s) on host SPI%d (CS=IO%d, MOSI=IO%d, MISO=IO%d, CLK=IO%d)",
                 mp(),
                 (int)s_spi_host, CONFIG_APP_SDMMC_D3, CONFIG_APP_SDMMC_CMD,
                 CONFIG_APP_SDMMC_D0, CONFIG_APP_SDMMC_CLK);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "SDSPI mount failed: %s", esp_err_to_name(err));
    // ensure bus is freed if mount failed
    spi_bus_free(s_spi_host);
    return err;
}

/* ---------- Public API ---------- */
esp_err_t sdcard_mount(sdmmc_card_t **out_card) {
    if (s_mounted) { if (out_card) *out_card = s_card; return ESP_OK; }

#if CONFIG_APP_SD_FORCE_SPI
    esp_err_t e = try_mount_sdspi();
#else
    esp_err_t e = try_mount_sdmmc();
    if (e != ESP_OK) e = try_mount_sdspi();
#endif

    if (e == ESP_OK && out_card) *out_card = s_card;
    return e;
}

esp_err_t sdcard_unmount(void) {
    if (!s_mounted) return ESP_OK;

    esp_err_t err = ESP_OK;
    if (!s_used_spi) {
        // SDMMC
        err = esp_vfs_fat_sdcard_unmount(mp(), s_card);
        // sdmmc host deinit for safety
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        if (host.deinit) host.deinit();
    } else {
        // SPI
        err = esp_vfs_fat_sdcard_unmount(mp(), s_card);
        spi_bus_free(s_spi_host);
    }
    s_card = NULL;
    s_mounted = false;
    s_used_spi = false;
    ESP_LOGI(TAG, "Unmounted %s", mp());
    return err;
}

bool sdcard_is_mounted(void) { return s_mounted; }

esp_err_t sdcard_write_test_file(const char *device_id) {
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen(CONFIG_APP_SD_MOUNT_POINT "/hello.txt", "a");
    if (!f) return ESP_FAIL;
    fprintf(f, "DeviceID=%s\n", device_id ? device_id : "(null)");
    fclose(f);
    ESP_LOGI(TAG, "Wrote %s/hello.txt", CONFIG_APP_SD_MOUNT_POINT);
    return ESP_OK;
}

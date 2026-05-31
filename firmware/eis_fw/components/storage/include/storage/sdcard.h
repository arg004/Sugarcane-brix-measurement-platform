#pragma once
#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mounts at CONFIG_APP_SD_MOUNT_POINT.
 *  Tries SDMMC (4-bit or 1-bit per Kconfig), then SPI fallback (same pins).
 *  Returns ESP_OK on success. */
esp_err_t sdcard_mount(sdmmc_card_t **out_card);

/** Unmount if mounted, and release the underlying host/bus. */
esp_err_t sdcard_unmount(void);

/** Quick helper to write /sdcard/hello.txt with DeviceID text. */
esp_err_t sdcard_write_test_file(const char *device_id);

/** Returns true if currently mounted. */
bool sdcard_is_mounted(void);

// NEW: Card-detect helpers
esp_err_t sdcard_cd_start(void);   // start CD ISR + task; auto-mount if enabled
void      sdcard_cd_stop(void);    // stop CD handling
bool      sdcard_is_inserted(void); // read raw CD GPIO with polarity


#ifdef __cplusplus
}
#endif

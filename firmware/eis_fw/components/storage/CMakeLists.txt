idf_component_register(
    SRCS
        "sd_storage.c"      
        "sdcard_cd.c"       # card detect
    INCLUDE_DIRS
        "include"
    REQUIRES
        vfs
        fatfs
        sdmmc
        esp_driver_sdmmc
        esp_driver_gpio
)
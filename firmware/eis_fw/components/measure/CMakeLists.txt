idf_component_register(
    SRCS
        "measure.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        storage          # sdcard_mount/unmount/is_mounted
        json             # cJSON
        mqtt             # esp-mqtt
        esp_event
        vfs              # fopen/fwrite
        config
        telemetry
        ems
)
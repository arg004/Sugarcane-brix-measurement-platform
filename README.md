# Sugarcane Brix Measurement Platform: Sensor Node

## Layout

```
board_easyeda/
  ProPrj_EIS-HW-V01_test_2026-05-23.epro   EasyEDA Pro project: schematic (sheets P1-P5) and PCB
firmware/eis_fw/                           ESP-IDF project for the ESP32-S3
  main/
    app_main.c      boot sequence; starts every component
    mux_test.c      bench test for the multiplexer select pins (not called)
  components/
    config/         device ID in NVS
    network/        Wi-Fi, reconnect, SNTP time sync
    telemetry/      shared MQTT client, heartbeat, online/offline status
    ems/            EmStat Pico UART link and EIS measurement
    mux/            TMUX1109 sensor selection
    regen/          regeneration and wash outputs
    storage/        microSD mount and card-detect
    measure/        test path that writes synthetic data
```

Each component has its sources, a public header under `include/<component>/`, and a `Kconfig` with its settings (pins, timings, topics).

## Where to find what

| Looking for | Go to | Paper term |
| --- | --- | --- |
| Boot order and startup | `main/app_main.c` | Boot and Configuration |
| Device ID generation | `config/nvs_config.c` | |
| Wi-Fi connection and reconnect backoff | `network/wifi.c`, `network/network_task.c` | Network and Time Service |
| Time sync and timestamps | `network/sntp_time.c` | |
| MQTT client creation, heartbeat, Last Will | `telemetry/heartbeat.c` (also implements `mqtt_bus.h`) | MQTT Bus and Telemetry |
| EIS measurement: sensor select, MethodSCRIPT, data parsing, CSV write | `ems/ems_eis.c` | EIS Run Task |
| The EIS MethodSCRIPT and sweep parameters | `kEIS_MSCR` in `ems/ems_eis.c` | |
| EmStat Pico UART driver | `ems/ems_uart.c` | UART Driver |
| EMS command topic and raw MethodSCRIPT commands | `ems/ems_cmd.c` | MQTT Control Plane (EMS domain) |
| Sensor-to-address mapping, parked channel | `mux/mux_tmux1109.c` (`mux.c` is not compiled) | TMUX1109 Driver |
| Regeneration and wash pulses | `regen/regen_ctrl.c` (wash = all three lines together) | Maintenance Worker |
| SD card mount (SDMMC with SPI fallback) | `storage/sd_storage.c` | Storage Driver |
| SD card hot-swap | `storage/sdcard_cd.c` | SD Card Manager |
| Synthetic test measurement | `measure/measure.c` | Measure domain |
| Pin assignments and default settings | `Kconfig` in each component | |
| Board schematic and PCB | `board_easyeda/` (open in EasyEDA Pro) | Sensor Node Hardware |

## MQTT topics

All topics are `devices/<device_id>/...`. Each component subscribes to its own command topic.

| Topic | Handled in |
| --- | --- |
| `commands/ems`, `status/ems` | `ems/ems_cmd.c`, `ems/ems_eis.c` |
| `commands/regen`, `status/regen` | `regen/regen_ctrl.c` |
| `commands/measure`, `status/measure` | `measure/measure.c` |
| `heartbeat`, `status` | `telemetry/heartbeat.c` |

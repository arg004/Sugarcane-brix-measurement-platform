# firmware/components/telemetry/CMakeLists.txt  (new)
idf_component_register(
  SRCS "heartbeat.c"
  INCLUDE_DIRS "include"
  REQUIRES mqtt esp_netif esp_event nvs_flash json app_update network config esp_wifi
  PRIV_REQUIRES log
)
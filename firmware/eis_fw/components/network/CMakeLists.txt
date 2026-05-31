idf_component_register(
  SRCS "wifi.c" "sntp_time.c" "network_task.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_wifi esp_netif esp_event lwip nvs_flash freertos
  PRIV_REQUIRES log
)
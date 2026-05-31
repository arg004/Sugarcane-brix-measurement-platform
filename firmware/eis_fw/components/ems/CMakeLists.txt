idf_component_register(
  SRCS "ems_uart.c" "ems_cmd.c" "ems_eis.c"
  INCLUDE_DIRS "include"
  PRIV_REQUIRES esp_driver_uart esp_timer esp_event mqtt config mux json telemetry storage
)
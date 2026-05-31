idf_component_register(
  SRCS "regen_ctrl.c"
  INCLUDE_DIRS "include"
  REQUIRES driver freertos mqtt json esp_event telemetry config
  PRIV_REQUIRES log
)
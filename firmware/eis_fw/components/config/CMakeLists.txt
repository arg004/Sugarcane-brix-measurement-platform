idf_component_register(
  SRCS "nvs_config.c"
  INCLUDE_DIRS "include"
  REQUIRES nvs_flash esp_system
  PRIV_REQUIRES log
)
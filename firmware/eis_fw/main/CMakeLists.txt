idf_component_register(SRCS "app_main.c" "mux_test.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash config network telemetry regen storage measure ems mux)

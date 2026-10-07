idf_component_register(
    SRCS "spi_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_driver_gpio esp_driver_spi
)

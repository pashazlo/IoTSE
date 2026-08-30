idf_component_register(
    SRCS "spi_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos
    REQUIRES driver esp_driver_spi
)

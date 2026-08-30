idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd freertos spi_bus
    REQUIRES driver esp_driver_spi esp_driver_gpio esp_lcd spi_bus
)

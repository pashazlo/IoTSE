idf_component_register(
    SRCS "main.c" "ui.c"
    INCLUDE_DIRS "." "assets"
    REQUIRES display spi_bus
    REQUIRES gfx_canva
)

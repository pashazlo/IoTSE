idf_component_register(
    SRCS "main.c" "ui.c"
    INCLUDE_DIRS "."
    REQUIRES display spi_bus
    REQUIRES gfx_canvas
)

idf_component_register(
    SRCS "main.c" "ui.c" "buttons.c"
    INCLUDE_DIRS "." "assets"
    REQUIRES display spi_bus gfx_canvas 
)

idf_component_register(
    SRCS "main.c"  
    INCLUDE_DIRS "." "assets" "ui_engine"
    REQUIRES display spi_bus gfx_canvas 
)

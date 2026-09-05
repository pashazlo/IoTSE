# Автоматически находим main.c и ВСЕ .c файлы внутри папки ui_engine
file(GLOB_RECURSE SRCS "main.c" "ui_engine/*.c")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS 
        "." 
        "assets" 
        "ui_engine"
    REQUIRES 
        buttons 
        display 
        gfx_canvas 
        spi_bus 
        driver 
        freertos
)

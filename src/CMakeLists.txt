# Автоматически собирает main.c и все .c файлы движка
file(GLOB_RECURSE SRCS "main.c" "ui_engine/*.c")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS 
        "." 
        "assets" 
        "ui_engine"
        "ui_engine/include"  # <-- Добавили папку с заголовочниками движка
    REQUIRES 
        buttons 
        display 
        gfx_canvas 
        spi_bus 
        driver 
        freertos
)

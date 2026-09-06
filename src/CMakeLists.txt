# Автоматически собирает main.c и все .c файлы движка
file(GLOB_RECURSE SRCS "main.c" "ui_engine/*.c""storage/*.c"")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS 
        "." 
        "assets" 
        "ui_engine"
        "storage"
        "storage/include"
        "ui_engine/include" 
        "ui_engine/widgets"
        "ui_engine/widgets/include"
    REQUIRES 
        buttons 
        display 
        gfx_canvas 
        spi_bus 
        driver 
        freertos
        
)

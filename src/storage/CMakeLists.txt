idf_component_register(
    SRCS "file_service.c" "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash fatfs wear_levelling
)

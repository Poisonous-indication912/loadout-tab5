idf_component_register(
    SRCS "fw_boot.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_partition
)

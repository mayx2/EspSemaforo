idf_component_register(
    SRCS "app_main.c" "wifi.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt nvs_flash esp_wifi driver esp_system json
)

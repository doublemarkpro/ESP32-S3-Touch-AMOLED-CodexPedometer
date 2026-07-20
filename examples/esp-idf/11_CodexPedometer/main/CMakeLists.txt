idf_component_register(
    SRCS main.c ui_icons.c
    INCLUDE_DIRS .
    PRIV_REQUIRES driver nvs_flash lvgl__lvgl waveshare__qmi8658 esp_wifi esp_http_client esp_http_server esp_netif esp_event mbedtls
)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -Wno-error=attributes
)

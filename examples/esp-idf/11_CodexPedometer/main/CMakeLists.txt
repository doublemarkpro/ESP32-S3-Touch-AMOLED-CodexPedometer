idf_component_register(
    SRCS main.c ui_icons.c fonts/app_font_cjk_16.c fonts/app_font_cjk_22.c
    INCLUDE_DIRS .
    PRIV_REQUIRES driver nvs_flash lvgl__lvgl waveshare__qmi8658 esp_wifi esp_http_client esp_http_server esp_netif esp_event mbedtls
)

# The generated font picks its LVGL header via this macro; without it the
# file falls back to "lvgl/lvgl.h", which is not on this component's path.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -Wno-error=attributes
)

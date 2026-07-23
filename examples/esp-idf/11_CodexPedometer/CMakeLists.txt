# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(codex_pedometer)

# Put LVGL's builtin heap (widgets, styles, labels, draw tasks) in PSRAM.
# The Kconfig cannot express a runtime-allocated pool, so inject the hooks
# lv_conf_internal.h honours. Internal RAM is the scarce resource on this
# board once Wi-Fi is up; the render buffers stay internal regardless.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PUBLIC
    "-DLV_MEM_POOL_INCLUDE=<esp_heap_caps.h>"
    "-DLV_MEM_POOL_ALLOC(size)=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)")

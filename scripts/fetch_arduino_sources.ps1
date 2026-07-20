$ErrorActionPreference = "Stop"

$ProjectRoot = "D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75"

git -C $ProjectRoot sparse-checkout set --no-cone `
    '/.gitignore' `
    '/CHANGELOG.md' `
    '/CONTRIBUTING.md' `
    '/LICENSE' `
    '/README.md' `
    '/SECURITY.md' `
    '/SUPPORT.md' `
    '/docs/**' `
    '/config/**' `
    '/Schematic/**' `
    '/scripts/**' `
    '/tools/**' `
    '/examples/arduino/01_HelloWorld/**' `
    '/examples/arduino/03_LVGL_PCF85063_simpleTime/**' `
    '/examples/arduino/04_LVGL_QMI8658_ui/**' `
    '/examples/arduino/05_LVGL_AXP2101_ADC_Data/**' `
    '/examples/arduino/10_Touch_CST9217/**' `
    '/examples/arduino/11_CodexPedometer/**' `
    '/examples/arduino/libraries/lv_conf.h' `
    '/examples/arduino/libraries/Mylibrary/**' `
    '/examples/arduino/libraries/ESP32_IO_Expander/library.properties' `
    '/examples/arduino/libraries/ESP32_IO_Expander/src/**' `
    '/examples/arduino/libraries/GFX_Library_for_Arduino/library.properties' `
    '/examples/arduino/libraries/GFX_Library_for_Arduino/src/**' `
    '/examples/arduino/libraries/SensorLib/library.properties' `
    '/examples/arduino/libraries/SensorLib/src/**' `
    '/examples/arduino/libraries/SensorLib/examples/QMI8658_GetDataExample/**' `
    '/examples/arduino/libraries/SensorLib/examples/QMI8658_PedometerExample/**' `
    '/examples/arduino/libraries/XPowersLib/library.properties' `
    '/examples/arduino/libraries/XPowersLib/src/**' `
    '/examples/arduino/libraries/lvgl/library.properties' `
    '/examples/arduino/libraries/lvgl/lvgl.h' `
    '/examples/arduino/libraries/lvgl/lv_conf_template.h' `
    '/examples/arduino/libraries/lvgl/src/**'

git -C $ProjectRoot status --short

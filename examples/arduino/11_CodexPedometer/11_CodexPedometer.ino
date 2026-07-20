#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <lvgl.h>

#include "Arduino_GFX_Library.h"
#include "SensorQMI8658.hpp"
#include "lv_conf.h"
#include "pin_config.h"

#define EXAMPLE_LVGL_TICK_PERIOD_MS 2

// Fill these in before uploading.
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Run tools/codex_usage_server.py on your PC, then replace the host with your PC LAN IP.
static const char *CODEX_USAGE_URL = "http://192.168.50.37:8765/usage";

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[LCD_WIDTH * LCD_HEIGHT / 10];

static lv_obj_t *steps_value_label;
static lv_obj_t *steps_subtitle_label;
static lv_obj_t *motion_label;
static lv_obj_t *codex_label;
static lv_obj_t *wifi_label;
static lv_obj_t *status_label;
static lv_obj_t *steps_bar;

static SensorQMI8658 qmi;
static bool qmi_ready = false;
static uint32_t steps = 0;
static uint32_t last_usage_fetch_ms = 0;
static uint32_t last_ui_update_ms = 0;
static String codex_line = "Codex: waiting for usage service";

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

#if LV_USE_LOG != 0
void my_print(const char *buf) {
  Serial.printf("%s", buf);
  Serial.flush();
}
#endif

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

  lv_disp_flush_ready(disp);
}

void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area) {
  if (area->x1 % 2 != 0) area->x1--;
  if (area->y1 % 2 != 0) area->y1--;
  if (area->x2 % 2 == 0) area->x2++;
  if (area->y2 % 2 == 0) area->y2++;
}

void example_increase_lvgl_tick(void *arg) {
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static String jsonStringValue(const String &json, const char *key, const char *fallback) {
  String marker = String("\"") + key + "\":";
  int start = json.indexOf(marker);
  if (start < 0) return fallback;
  start += marker.length();
  while (start < json.length() && isspace(json[start])) start++;
  if (start >= json.length() || json[start] != '"') return fallback;
  start++;
  int end = json.indexOf('"', start);
  if (end < 0) return fallback;
  return json.substring(start, end);
}

static long jsonLongValue(const String &json, const char *key, long fallback) {
  String marker = String("\"") + key + "\":";
  int start = json.indexOf(marker);
  if (start < 0) return fallback;
  start += marker.length();
  while (start < json.length() && isspace(json[start])) start++;
  int end = start;
  while (end < json.length() && (isdigit(json[end]) || json[end] == '-')) end++;
  if (end == start) return fallback;
  return json.substring(start, end).toInt();
}

static void setLabel(lv_obj_t *label, const String &text) {
  lv_label_set_text(label, text.c_str());
}

static void createLabel(lv_obj_t **label, lv_obj_t *parent, const char *text, const lv_font_t *font,
                        lv_align_t align, int x, int y, lv_color_t color) {
  *label = lv_label_create(parent);
  lv_label_set_text(*label, text);
  lv_obj_set_style_text_font(*label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(*label, color, LV_PART_MAIN);
  lv_obj_set_width(*label, 400);
  lv_label_set_long_mode(*label, LV_LABEL_LONG_WRAP);
  lv_obj_align(*label, align, x, y);
}

static void buildUi() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x05070A), LV_PART_MAIN);

  createLabel(&status_label, screen, "ESP32-S3 Touch AMOLED", &lv_font_montserrat_20,
              LV_ALIGN_TOP_MID, 0, 26, lv_color_hex(0x9CA3AF));
  createLabel(&steps_subtitle_label, screen, "Steps today", &lv_font_montserrat_24,
              LV_ALIGN_TOP_MID, 0, 82, lv_color_hex(0xA7F3D0));
  createLabel(&steps_value_label, screen, "0", &lv_font_montserrat_48,
              LV_ALIGN_TOP_MID, 0, 122, lv_color_hex(0xF8FAFC));

  steps_bar = lv_bar_create(screen);
  lv_obj_set_size(steps_bar, 320, 18);
  lv_obj_align(steps_bar, LV_ALIGN_TOP_MID, 0, 202);
  lv_bar_set_range(steps_bar, 0, 10000);
  lv_bar_set_value(steps_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(steps_bar, lv_color_hex(0x1F2937), LV_PART_MAIN);
  lv_obj_set_style_bg_color(steps_bar, lv_color_hex(0x22C55E), LV_PART_INDICATOR);

  createLabel(&motion_label, screen, "IMU: initializing", &lv_font_montserrat_20,
              LV_ALIGN_TOP_MID, 0, 250, lv_color_hex(0xE5E7EB));
  createLabel(&codex_label, screen, "Codex: waiting for usage service", &lv_font_montserrat_20,
              LV_ALIGN_TOP_MID, 0, 306, lv_color_hex(0xC4B5FD));
  createLabel(&wifi_label, screen, "Wi-Fi: not connected", &lv_font_montserrat_16,
              LV_ALIGN_BOTTOM_MID, 0, -34, lv_color_hex(0x93C5FD));
}

static bool initPedometer() {
  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("QMI8658 not found");
    return false;
  }

  Serial.printf("QMI8658 chip id: 0x%02X\n", qmi.getChipID());
  qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_2G, SensorQMI8658::ACC_ODR_62_5Hz);
  qmi.enableAccelerometer();

  qmi.configPedometer(
    50,   // sample count
    200,  // valid peak-to-peak threshold, mg
    100,  // peak threshold over average, mg
    200,  // timeout window, samples
    20,   // quiet time, samples
    10,   // continuous steps before count becomes valid
    0,    // recommended precision
    4     // update output registers every 4 valid steps
  );

  if (!qmi.enablePedometer(SensorQMI8658::INTERRUPT_PIN_DISABLE)) {
    Serial.println("QMI8658 pedometer enable failed");
    return false;
  }

  return true;
}

static void connectWiFi() {
  if (String(WIFI_SSID) == "YOUR_WIFI_SSID") {
    setLabel(wifi_label, "Wi-Fi: set SSID/password in sketch");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  setLabel(wifi_label, "Wi-Fi: connecting...");
}

static void refreshCodexUsage() {
  if (WiFi.status() != WL_CONNECTED) {
    codex_line = "Codex: Wi-Fi offline";
    return;
  }

  HTTPClient http;
  http.setTimeout(2500);
  if (!http.begin(CODEX_USAGE_URL)) {
    codex_line = "Codex: bad usage URL";
    return;
  }

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    String label = jsonStringValue(payload, "label", "Codex");
    long used = jsonLongValue(payload, "used_tokens", -1);
    long limit = jsonLongValue(payload, "limit_tokens", -1);
    String updated = jsonStringValue(payload, "updated", "");

    if (used >= 0 && limit > 0) {
      codex_line = label + ": " + String(used / 1000.0, 1) + "k / " + String(limit / 1000.0, 0) + "k tok";
    } else if (used >= 0) {
      codex_line = label + ": " + String(used) + " tok";
    } else {
      codex_line = label + ": " + payload;
    }
    if (updated.length() > 0) codex_line += "\nUpdated: " + updated;
  } else {
    codex_line = "Codex: HTTP " + String(code);
  }

  http.end();
}

static void updateSensors() {
  if (!qmi_ready) return;

  qmi.update();
  steps = qmi.getPedometerCounter();

  IMUdata acc;
  if (qmi.getAccelerometer(acc.x, acc.y, acc.z)) {
    String motion = "ACC g  X " + String(acc.x, 2) +
                    "  Y " + String(acc.y, 2) +
                    "  Z " + String(acc.z, 2);
    setLabel(motion_label, motion);
  }
}

static void updateUi() {
  setLabel(steps_value_label, String(steps));
  lv_bar_set_value(steps_bar, steps > 10000 ? 10000 : steps, LV_ANIM_ON);

  if (qmi_ready) {
    setLabel(status_label, "QMI8658 pedometer running");
  } else {
    setLabel(status_label, "QMI8658 unavailable");
  }

  if (WiFi.status() == WL_CONNECTED) {
    setLabel(wifi_label, "Wi-Fi: " + WiFi.localIP().toString());
  }

  setLabel(codex_label, codex_line);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(IIC_SDA, IIC_SCL);
  gfx->begin();
  gfx->setBrightness(180);

  lv_init();
#if LV_USE_LOG != 0
  lv_log_register_print_cb(my_print);
#endif

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, LCD_WIDTH * LCD_HEIGHT / 10);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_WIDTH;
  disp_drv.ver_res = LCD_HEIGHT;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.rounder_cb = example_lvgl_rounder_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  lv_indev_drv_register(&indev_drv);

  const esp_timer_create_args_t lvgl_tick_timer_args = {
    .callback = &example_increase_lvgl_tick,
    .name = "lvgl_tick"
  };
  esp_timer_handle_t lvgl_tick_timer = NULL;
  esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
  esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);

  buildUi();
  qmi_ready = initPedometer();
  connectWiFi();
  updateUi();
}

void loop() {
  lv_timer_handler();

  const uint32_t now = millis();
  if (now - last_ui_update_ms >= 250) {
    last_ui_update_ms = now;
    updateSensors();
    updateUi();
  }

  if (now - last_usage_fetch_ms >= 30000) {
    last_usage_fetch_ms = now;
    refreshCodexUsage();
  }

  delay(5);
}

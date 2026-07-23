#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lvgl.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"

#include "app_config.h"
#include "ui_icons.h"

/* qmi8658.h defines M_PI; IDF v6 picolibc already provides it. */
#ifdef M_PI
#undef M_PI
#endif
#include "qmi8658.h"

#define DISPLAY_LOCK_TIMEOUT_MS 200
#define RESET_BUTTON_GPIO GPIO_NUM_0

/* Draw buffers must live in internal RAM: the ESP32-S3 SPI driver cannot DMA
 * from PSRAM and would malloc an internal bounce buffer per flush, which
 * starts failing once Wi-Fi has claimed internal heap. 466 x 20 x 2 bytes
 * x 2 buffers = ~26 KB reserved once at boot. Height is kept modest so task
 * stacks (which must be internal) always fit alongside the LVGL widgets. */
#define APP_DRAW_BUFFER_HEIGHT 20
#define CO5300_GRAM_RES 480
#define PANEL_CLEAR_ROWS 13

/* Whole-UI rotation is done in the CO5300 panel (MADCTL) so it costs nothing.
 * The touch flags below must match the panel orientation. */
#define APP_DISPLAY_ROTATION BSP_DISPLAY_ROTATE_90
#define APP_TOUCH_SWAP_XY 1
#define APP_TOUCH_MIRROR_X 0
#define APP_TOUCH_MIRROR_Y 1

/* Pull-down settings panel with persisted UI preferences. */
#define SETTINGS_NVS_NAMESPACE "ui_cfg"
#define SETTINGS_NVS_BRIGHTNESS_KEY "bright"
#define SETTINGS_NVS_VOLUME_KEY "volume"
#define SETTINGS_NVS_STANDBY_EN_KEY "stby_en"
#define SETTINGS_NVS_STANDBY_MIN_KEY "stby_min"
#define SETTINGS_BRIGHTNESS_MIN 10
#define SETTINGS_STANDBY_MIN_MINUTES 1
#define SETTINGS_STANDBY_MAX_MINUTES 10

/* Infograph-style watch face geometry (466 x 466 round panel). */
#define WATCH_CENTER_X (BSP_LCD_H_RES / 2)
#define WATCH_CENTER_Y (BSP_LCD_V_RES / 2)
#define WATCH_TICK_OUTER_R 226
#define WATCH_TICK_MINOR_R 212
#define WATCH_TICK_MAJOR_R 203
#define WATCH_NUMERAL_R 172
#define WATCH_HOUR_HAND_LEN 95.0f
#define WATCH_MINUTE_HAND_LEN 145.0f
#define WATCH_SECOND_HAND_LEN 163.0f
#define WATCH_HAND_TAIL_LEN 20.0f
#define WATCH_SECOND_TAIL_LEN 34.0f
#define WATCH_GAUGE_SIZE 56
#define WATCH_CORNER_OFS 115
#define WATCH_DEG_TO_RAD(deg) ((float)(deg) * (float)M_PI / 180.0f)

#define WATCH_COLOR_TICK_MAJOR 0xE5E5EA
#define WATCH_COLOR_TICK_MINOR 0x5A5A5E
#define WATCH_COLOR_HAND 0xF2F2F7
#define WATCH_COLOR_SECOND 0xFF9F0A
#define WATCH_COLOR_BATTERY 0x30D158
#define WATCH_COLOR_TEMP 0xFF9F0A
#define WATCH_COLOR_WEEKDAY 0xFFD60A
#define WATCH_COLOR_MONTH 0xFF453A
#define WATCH_COLOR_DIGITAL 0x98989D
#define WATCH_COLOR_CAPTION 0x8E8E93
#define WATCH_COLOR_GAUGE_BG 0x26292E

#define BATTERY_POLL_MS 30000
#define TEMP_POLL_TICKS 125
#define AXP2101_I2C_ADDR 0x34
#define AXP2101_REG_BATT_PERCENT 0xA4
#define BOARD_TEMP_INVALID (-1000.0f)

#define PEDOMETER_DEFAULT_GOAL_STEPS 12000U
#define PEDOMETER_MIN_GOAL_STEPS 1000U
#define PEDOMETER_MAX_GOAL_STEPS 100000U
#define PEDOMETER_TASK_DELAY_MS 40
#define UI_REFRESH_MS 250

#define STEP_MIN_INTERVAL_MS 300U
#define STEP_PEAK_THRESHOLD_G 0.18f
#define STEP_RESET_THRESHOLD_G 0.04f
#define GRAVITY_SMOOTHING_ALPHA 0.08f

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WEATHER_REFRESH_REQUEST_BIT BIT2
#define CODEX_REFRESH_REQUEST_BIT BIT3
/* Short immediate-retry burst, then long backoff: every STA connect attempt
 * drags the shared radio off the setup-AP channel for seconds, which is what
 * makes the CodexPedometer hotspot undiscoverable. */
#define WIFI_MAXIMUM_RETRY 3
#define WIFI_RETRY_BACKOFF_MS 30000
#define WIFI_SCAN_CACHE_MS 60000
#define WIFI_SETUP_AP_SSID "CodexPedometer"
#define WIFI_SETUP_AP_PASSWORD ""
#define WIFI_SETUP_AP_CHANNEL 6
#define WIFI_SETUP_AP_MAX_CONN 4
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "pass"
#define WEATHER_NVS_CITY_KEY "weather_city"
#define WEATHER_NVS_KEY_KEY "weather_key"
#define PEDOMETER_NVS_GOAL_KEY "step_goal"
#define WIFI_SSID_STORAGE_SIZE 33
#define WIFI_PASSWORD_STORAGE_SIZE 65
#define WEATHER_CITY_STORAGE_SIZE 64
#define WEATHER_KEY_STORAGE_SIZE 80
#define WIFI_SCAN_MAX_AP 16

/* 100 ms tick gives a sweeping second hand; labels and the other hands
 * still refresh once per second. */
#define CLOCK_TASK_DELAY_MS 100
#define NTP_RETRY_INTERVAL_MS 30000
#define DEFAULT_TIME_YEAR 2026
#define DEFAULT_TIME_MONTH 7
#define DEFAULT_TIME_DAY 20
#define DEFAULT_TIME_HOUR 8
#define DEFAULT_TIME_MINUTE 0
#define DEFAULT_TIME_SECOND 0
#define DEFAULT_TIMEZONE "CST-8"

#define CODEX_POLL_INTERVAL_MS 15000
#define CODEX_HTTP_TIMEOUT_MS 5000
/* The agent page stands in for a status lamp, so it polls far more often
 * than the quota page. */
#define AGENT_POLL_INTERVAL_MS 2000
#define AGENT_HTTP_TIMEOUT_MS 3000

/* Music visualizer: mic -> 512-pt FFT -> 18 log-spaced bands. One 32 ms
 * capture block per frame gives ~31 updates/s. The mic only runs while the
 * music page is showing. */
#define MUSIC_SAMPLE_RATE 16000
#define MUSIC_FFT_SIZE 512
#define MUSIC_BANDS 18
#define MUSIC_BAR_MAX_H 150
#define MUSIC_BAR_WIDTH 10
#define MUSIC_BAR_PITCH 15
#define MUSIC_BASELINE_Y 336
#define MUSIC_MIC_GAIN_DB 30.0f
#define MUSIC_SLOW_UI_DIVIDER 8
/* AMap returns today plus three forecast days in one call. */
#define WEATHER_FORECAST_DAYS 4
#define WEATHER_POLL_INTERVAL_MS 1800000
#define WEATHER_RETRY_INTERVAL_MS 60000
#define WEATHER_HTTP_TIMEOUT_MS 8000
#define HTTP_RESPONSE_BUFFER_SIZE 4096
#define SCREENSHOT_CMD_TASK_STACK 3072
#define DEBUG_AUTO_DELAY_MS 5000
#define UI_ROUND_VISIBLE_RADIUS (BSP_LCD_H_RES / 2)

/* Data pages: a 440 px progress arc hugs the round bezel; its 90-degree
 * bottom opening hosts the page dots. Metrics sit in three open columns. */
#define UI_ARC_SIZE 440
#define UI_ARC_TOP 13
#define UI_ARC_WIDTH 14
#define UI_LABEL_SCALE_NONE 256
#define UI_MAIN_ICON_CENTER_Y -120
#define UI_CAPTION_CENTER_Y -70
#define UI_VALUE_CENTER_Y -8
#define UI_GOAL_CENTER_Y 52
#define UI_STATUS_CENTER_Y 86
#define UI_METRIC_COL_OFS 117
#define UI_METRIC_ICON_CENTER_Y 107
#define UI_METRIC_VALUE_CENTER_Y 133
#define UI_METRIC_LABEL_CENTER_Y 155
#define UI_UPDATED_CENTER_Y 172
/* Agent page keeps its timestamp directly under the state word. */
#define UI_AGENT_UPDATED_CENTER_Y 26
#define PAGE_DOTS_VISIBLE 0
#define PAGE_NAV_DEBOUNCE_MS 320
#define PAGE_ANIM_MS 240
#define PAGE_SWIPE_MIN_PX 45
/* Holding a page this long (without moving) forces a manual sync: NTP on the
 * clock page, weather refresh on the weather page. */
#define PAGE_LONG_PRESS_MS 800
#define PAGE_SWIPE_MAX_OFF_AXIS_PX 90

#if LV_FONT_MONTSERRAT_48
#define FONT_STEPS (&lv_font_montserrat_48)
#else
#define FONT_STEPS LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_32
#define FONT_VALUE (&lv_font_montserrat_32)
#else
#define FONT_VALUE LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_24
#define FONT_TITLE (&lv_font_montserrat_24)
#else
#define FONT_TITLE LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_20
#define FONT_MEDIUM (&lv_font_montserrat_20)
#else
#define FONT_MEDIUM LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_14
#define FONT_SMALL (&lv_font_montserrat_14)
#else
#define FONT_SMALL LV_FONT_DEFAULT
#endif

#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
#define FONT_CJK (&lv_font_source_han_sans_sc_16_cjk)
#elif LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
#define FONT_CJK (&lv_font_source_han_sans_sc_14_cjk)
#else
#define FONT_CJK FONT_SMALL
#endif

typedef enum {
    APP_PAGE_TIME,
    APP_PAGE_WEATHER,
    APP_PAGE_PEDOMETER,
    APP_PAGE_CODEX,
    APP_PAGE_AGENT,
    APP_PAGE_MUSIC,
    APP_PAGE_COUNT,
} app_page_t;

/* Mirrors the AgentCore-Light state model so this page can stand in for a
 * three-colour AI status lamp: green = idle/done, yellow = busy, amber =
 * waiting on the user, red = the turn errored. */
typedef enum {
    AGENT_STATE_UNKNOWN,
    AGENT_STATE_IDLE,
    AGENT_STATE_WORKING,
    AGENT_STATE_WAITING,
    AGENT_STATE_ERROR,
    AGENT_STATE_DONE,
} agent_state_t;

typedef enum {
    METRIC_ICON_PIN,
    METRIC_ICON_FLAME,
    METRIC_ICON_HEART,
    METRIC_ICON_USED,
    METRIC_ICON_LIMIT,
    METRIC_ICON_LEFT,
} metric_icon_t;

typedef struct {
    lv_obj_t *time_label;
    lv_obj_t *title_label;
    lv_obj_t *clock_icon;
    lv_obj_t *separator;
    lv_obj_t *battery_icon;
} status_ui_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *hour_hand;
    lv_obj_t *minute_hand;
    lv_obj_t *second_hand;
    lv_obj_t *digital_label;
    lv_obj_t *status_label;
    lv_obj_t *wifi_icon;
    lv_obj_t *ntp_icon;
    lv_obj_t *corner_arc[4];
    lv_obj_t *corner_value[4];
    lv_obj_t *corner_caption[4];
} time_ui_t;

/* Tap-cyclable corner complications on the watch face. Order here is the
 * cycle order. */
typedef enum {
    CW_BATTERY,
    CW_TEMP,
    CW_WEEKDAY,
    CW_DATE,
    CW_WEATHER,
    CW_CODEX,
    CW_AGENT,
    CW_COUNT,
} corner_widget_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *temp_arc;
    lv_obj_t *icon_bg;
    lv_obj_t *city_label;
    lv_obj_t *temp_label;
    lv_obj_t *condition_label;
    lv_obj_t *range_label;
    lv_obj_t *status_label;
    lv_obj_t *low_value_label;
    lv_obj_t *high_value_label;
    lv_obj_t *code_value_label;
    lv_obj_t *updated_label;
} weather_ui_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *progress_arc;
    lv_obj_t *main_icon;
    lv_obj_t *steps_label;
    lv_obj_t *goal_label;
    lv_obj_t *status_label;
    lv_obj_t *distance_value_label;
    lv_obj_t *calories_value_label;
    lv_obj_t *motion_value_label;
} pedometer_ui_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *usage_arc;
    lv_obj_t *main_icon;
    lv_obj_t *percent_label;
    lv_obj_t *label_label;
    lv_obj_t *status_label;
    lv_obj_t *used_value_label;
    lv_obj_t *limit_value_label;
    lv_obj_t *left_value_label;
    lv_obj_t *updated_label;
} codex_ui_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *ring;
    lv_obj_t *title_label;
    lv_obj_t *state_label;
    lv_obj_t *detail_label;
    lv_obj_t *target_label;
    lv_obj_t *codex_value_label;
    lv_obj_t *claude_value_label;
    lv_obj_t *elapsed_value_label;
    lv_obj_t *updated_label;
} agent_ui_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *range_arc;
    lv_obj_t *icon;
    lv_obj_t *day_label;
    lv_obj_t *date_label;
    lv_obj_t *range_label;
    lv_obj_t *cond_label;
    lv_obj_t *night_value_label;
    lv_obj_t *wind_value_label;
    lv_obj_t *force_value_label;
    lv_obj_t *dots[WEATHER_FORECAST_DAYS];
} forecast_ui_t;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *vu_arc;
    lv_obj_t *bars[MUSIC_BANDS];
    lv_obj_t *peaks[MUSIC_BANDS];
    lv_obj_t *bass_value_label;
    lv_obj_t *vol_value_label;
    lv_obj_t *treb_value_label;
} music_ui_t;

typedef struct {
    agent_state_t state;
    agent_state_t codex_state;
    agent_state_t claude_state;
    char agent[24];
    char project[40];
    char detail[48];
    char updated[24];
    uint32_t elapsed_s;
    bool valid;
} agent_status_t;

typedef struct {
    float gravity_g;
    bool armed;
    uint32_t last_step_ms;
} pedometer_filter_t;

typedef struct {
    char label[40];
    uint64_t used_tokens;
    uint64_t limit_tokens;
    char updated[40];
} codex_usage_t;

typedef struct {
    char date[12];      /* MM-DD */
    char day_cond[24];
    char wind_dir[8];   /* compass letters, e.g. "S" */
    char wind_power[12];/* Beaufort range, e.g. "1-3" */
    int day_temp_c;
    int night_temp_c;
    int day_code;
    int night_code;
    bool valid;
} weather_day_t;

typedef struct {
    char city[WEATHER_CITY_STORAGE_SIZE];
    char region[WEATHER_CITY_STORAGE_SIZE];
    char condition[32];
    char updated[40];
    int weather_code;
    int current_temp_c;
    int min_temp_c;
    int max_temp_c;
    bool valid;
    weather_day_t days[WEATHER_FORECAST_DAYS];
    int day_count;
} weather_data_t;

typedef struct {
    char body[HTTP_RESPONSE_BUFFER_SIZE];
    int length;
} http_response_t;

typedef struct {
    char ssid[WIFI_SSID_STORAGE_SIZE];
    char password[WIFI_PASSWORD_STORAGE_SIZE];
} wifi_credentials_t;

static const char *TAG = "codex_pedometer";
static status_ui_t s_status_ui;
static time_ui_t s_time_ui;
static weather_ui_t s_weather_ui;
static pedometer_ui_t s_pedometer_ui;
static codex_ui_t s_codex_ui;
static agent_ui_t s_agent_ui;
static music_ui_t s_music_ui;
static forecast_ui_t s_forecast_ui;
/* Forecast detail overlays the weather page rather than joining the page
 * carousel, so it does not lengthen the swipe chain for everyone else. */
static bool s_forecast_open;
static int s_forecast_day;
static agent_status_t s_agent_status = {
    .state = AGENT_STATE_UNKNOWN,
    .codex_state = AGENT_STATE_UNKNOWN,
    .claude_state = AGENT_STATE_UNKNOWN,
    .agent = "",
    .project = "",
    .detail = "",
    .updated = "",
    .elapsed_s = 0,
    .valid = false,
};
static lv_obj_t *s_dot_time;
static lv_obj_t *s_dot_weather;
static lv_obj_t *s_dot_pedometer;
static lv_obj_t *s_dot_codex;
static app_page_t s_current_page = APP_PAGE_TIME;
static uint32_t s_last_page_nav_ms;
static lv_point_t s_page_press_point;
static lv_point_t s_page_last_point;
static bool s_page_press_active;
static uint32_t s_page_press_start_ms;

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num;
static bool s_wifi_started;
static bool s_sntp_started;
static volatile bool s_time_synced;
static volatile bool s_ntp_sync_in_progress;
static uint32_t s_last_ntp_attempt_ms;
static httpd_handle_t s_config_server;
static wifi_credentials_t s_wifi_credentials;
static volatile int s_ap_client_count;
static wifi_ap_record_t s_wifi_scan_cache[WIFI_SCAN_MAX_AP];
static uint16_t s_wifi_scan_cache_count;
static uint32_t s_wifi_scan_cache_ms;
static char s_weather_city[WEATHER_CITY_STORAGE_SIZE];
static char s_weather_api_key[WEATHER_KEY_STORAGE_SIZE];
static uint32_t s_pedometer_goal_steps = PEDOMETER_DEFAULT_GOAL_STEPS;

static volatile bool s_reset_requested;
static uint32_t s_step_count;
static lv_point_precise_t s_hour_hand_points[2];
static lv_point_precise_t s_minute_hand_points[2];
static lv_point_precise_t s_second_hand_points[2];
static volatile int s_battery_percent = -1;
static volatile float s_board_temp_c = BOARD_TEMP_INVALID;
static volatile bool s_ntp_resync_requested;
static uint8_t s_corner_widget[4] = {CW_BATTERY, CW_TEMP, CW_WEEKDAY, CW_DATE};

/* Ring accent colors, adjustable from the provisioning web page. The AI ring
 * is deliberately absent: its color IS the state. */
typedef enum {
    THEME_BATTERY,
    THEME_TEMP,
    THEME_WEEKDAY,
    THEME_DATE,
    THEME_CODEX,
    THEME_STEPS,
    THEME_MUSIC,
    THEME_WEATHER,
    THEME_COUNT,
} theme_color_id_t;

static const char *const THEME_NAMES[THEME_COUNT] = {
    "电量", "温度", "星期", "日期", "Codex", "步数", "音乐", "天气",
};
static uint32_t s_theme_color[THEME_COUNT] = {
    0x30D158, 0xFF9F0A, 0xBF5AF2, 0xFF453A, 0xFFD166, 0x9DFF35, 0x18D7F5, 0x64D2FF,
};

typedef struct {
    lv_obj_t *panel;
    lv_obj_t *brightness_label;
    lv_obj_t *brightness_slider;
    lv_obj_t *volume_label;
    lv_obj_t *volume_slider;
    lv_obj_t *standby_checkbox;
    lv_obj_t *standby_slider;
} settings_ui_t;

static settings_ui_t s_settings_ui;
static bool s_settings_visible;
static lv_point_t s_settings_press_point;
static bool s_settings_press_active;
static uint8_t s_cfg_brightness = 80;
static uint8_t s_cfg_volume = 60;
static bool s_cfg_standby_enabled;
static uint8_t s_cfg_standby_minutes = 3;
static volatile uint32_t s_last_activity_ms;
static volatile bool s_screen_off;
static i2c_master_dev_handle_t s_axp2101_dev;
static codex_usage_t s_last_codex_usage = {
    .label = "Codex usage",
    .used_tokens = 0,
    .limit_tokens = 500000,
    .updated = "boot",
};
static weather_data_t s_last_weather = {
    .city = WEATHER_CITY,
    .region = "中国",
    .condition = "等待天气",
    .updated = "boot",
    .weather_code = 0,
    .current_temp_c = 0,
    .min_temp_c = 0,
    .max_temp_c = 0,
    .valid = false,
};

static void page_nav_event_cb(lv_event_t *event);
static void set_active_page_locked(app_page_t page);
static void render_time_page(void);
static void render_weather_status(const char *status_text);
static esp_err_t request_ntp_sync(void);
static void show_settings_panel(bool show);
static void agent_refresh_lamp(void);
static bool text_glyphs_available(const lv_font_t *font, const char *text);
static const char *weather_condition_en(int code);
static uint32_t agent_state_color(agent_state_t state);
static const char *agent_state_short(agent_state_t state);
static void save_ui_settings(void);
static bool corner_handle_tap(lv_point_t point);
static bool weather_icon_hit(lv_point_t point);
static void show_forecast_page(bool show);
static void render_forecast_locked(void);
static void page_nav_event_cb(lv_event_t *event);
static lv_obj_t *create_weather_icon_visual(lv_obj_t *parent);
static void draw_weather_icon(lv_obj_t *canvas, int code);
static bool json_get_string(const char *json, const char *key, char *out_value, size_t out_size);
static bool json_get_u64(const char *json, const char *key, uint64_t *out_value);
static esp_err_t http_get_url(const char *url, int timeout_ms, bool use_crt_bundle,
                              http_response_t *response);
static void render_pedometer(uint32_t steps, int motion_mg, const char *status_text);
static void update_time_page_locked(void);
static void screenshot_console_task(void *arg);

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t clamp_u32(uint32_t value, uint32_t max_value)
{
    return value > max_value ? max_value : value;
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static bool config_value_is_set(const char *value, const char *placeholder)
{
    return value != NULL && value[0] != '\0' && strcmp(value, placeholder) != 0;
}

static bool wifi_configured(void)
{
    return s_wifi_credentials.ssid[0] != '\0';
}

/* The placeholder hostname must match app_config.h exactly, otherwise an
 * unconfigured URL is "configured" as far as this check can tell and the UI
 * reports a misleading "Bridge offline" instead of asking for setup. */
static bool codex_configured(void)
{
    return wifi_configured() &&
           config_value_is_set(CODEX_USAGE_URL, "http://YOUR_PC_LAN_IP:8765/usage");
}

static bool agent_status_configured(void)
{
    return wifi_configured() &&
           config_value_is_set(AGENT_STATUS_URL, "http://YOUR_PC_LAN_IP:8766/status");
}

static bool weather_api_key_configured(void)
{
    return config_value_is_set(s_weather_api_key, "YOUR_AMAP_WEB_SERVICE_KEY");
}

static void copy_string(char *dest, size_t dest_size, const char *src)
{
    if (dest_size == 0) {
        return;
    }
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, dest_size, "%s", src);
}

static bool has_non_ascii(const char *text);

static void normalize_weather_city(char *city, size_t city_size)
{
    if (city == NULL || city_size == 0 || city[0] == '\0') {
        return;
    }

    /* Keep old/bad saved values from making weather city lookup resolve to the
     * wrong place. */
    if (strcmp(city, "青口") == 0 || strcmp(city, "青口市") == 0) {
        copy_string(city, city_size, "青岛市");
    }
}

typedef struct {
    const char *match_a;
    const char *match_b;
    const char *match_c;
    const char *display;
    const char *city_code;
    double latitude;
    double longitude;
} weather_city_preset_t;

static const weather_city_preset_t s_weather_city_presets[] = {
    {"青岛", "青岛市", "Qingdao", "Qingdao", "370200", 36.06488, 120.38042},
    {"青口", "青口市", NULL, "Qingdao", "370200", 36.06488, 120.38042},
    {"上海", "上海市", "Shanghai", "Shanghai", "310000", 31.23040, 121.47370},
    {"北京", "北京市", "Beijing", "Beijing", "110000", 39.90420, 116.40740},
    {"深圳", "深圳市", "Shenzhen", "Shenzhen", "440300", 22.54310, 114.05790},
    {"广州", "广州市", "Guangzhou", "Guangzhou", "440100", 23.12910, 113.26440},
    {"杭州", "杭州市", "Hangzhou", "Hangzhou", "330100", 30.27410, 120.15510},
    {"南京", "南京市", "Nanjing", "Nanjing", "320100", 32.06030, 118.79690},
    {"济南", "济南市", "Jinan", "Jinan", "370100", 36.65120, 117.12010},
};

static bool weather_city_matches(const char *city, const weather_city_preset_t *preset)
{
    return city != NULL &&
           ((preset->match_a != NULL && strcmp(city, preset->match_a) == 0) ||
            (preset->match_b != NULL && strcmp(city, preset->match_b) == 0) ||
            (preset->match_c != NULL && strcmp(city, preset->match_c) == 0));
}

static bool lookup_weather_city_preset(const char *city, double *latitude, double *longitude,
                                       char *display, size_t display_size)
{
    for (size_t i = 0; i < sizeof(s_weather_city_presets) / sizeof(s_weather_city_presets[0]);
         i++) {
        if (weather_city_matches(city, &s_weather_city_presets[i])) {
            *latitude = s_weather_city_presets[i].latitude;
            *longitude = s_weather_city_presets[i].longitude;
            copy_string(display, display_size, s_weather_city_presets[i].display);
            return true;
        }
    }
    return false;
}

static bool looks_like_weather_city_code(const char *city)
{
    if (city == NULL || strlen(city) != 6) {
        return false;
    }
    for (size_t i = 0; city[i] != '\0'; i++) {
        if (!isdigit((unsigned char)city[i])) {
            return false;
        }
    }
    return true;
}

static bool lookup_weather_city_code(const char *city, char *city_code, size_t city_code_size,
                                     char *display, size_t display_size)
{
    for (size_t i = 0; i < sizeof(s_weather_city_presets) / sizeof(s_weather_city_presets[0]);
         i++) {
        if (weather_city_matches(city, &s_weather_city_presets[i])) {
            copy_string(city_code, city_code_size, s_weather_city_presets[i].city_code);
            copy_string(display, display_size, s_weather_city_presets[i].display);
            return true;
        }
    }

    if (looks_like_weather_city_code(city)) {
        copy_string(city_code, city_code_size, city);
        copy_string(display, display_size, "China City");
        return true;
    }

    copy_string(city_code, city_code_size, s_weather_city_presets[0].city_code);
    copy_string(display, display_size, s_weather_city_presets[0].display);
    return true;
}

static void build_weather_display_city(const char *city, char *out, size_t out_size)
{
    double unused_latitude = 0.0;
    double unused_longitude = 0.0;
    if (lookup_weather_city_preset(city, &unused_latitude, &unused_longitude, out, out_size)) {
        return;
    }
    if (has_non_ascii(city)) {
        copy_string(out, out_size, "China City");
    } else {
        copy_string(out, out_size, city != NULL && city[0] != '\0' ? city : "Weather");
    }
}

static void request_weather_refresh(void)
{
    if (s_wifi_event_group != NULL) {
        xEventGroupSetBits(s_wifi_event_group, WEATHER_REFRESH_REQUEST_BIT);
    }
}

static void request_codex_refresh(void)
{
    if (s_wifi_event_group != NULL) {
        xEventGroupSetBits(s_wifi_event_group, CODEX_REFRESH_REQUEST_BIT);
    }
}

static void load_ui_settings(void)
{
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    uint8_t value = 0;
    if (nvs_get_u8(nvs, SETTINGS_NVS_BRIGHTNESS_KEY, &value) == ESP_OK &&
        value >= SETTINGS_BRIGHTNESS_MIN && value <= 100) {
        s_cfg_brightness = value;
    }
    if (nvs_get_u8(nvs, SETTINGS_NVS_VOLUME_KEY, &value) == ESP_OK && value <= 100) {
        s_cfg_volume = value;
    }
    if (nvs_get_u8(nvs, SETTINGS_NVS_STANDBY_EN_KEY, &value) == ESP_OK) {
        s_cfg_standby_enabled = value != 0;
    }
    if (nvs_get_u8(nvs, SETTINGS_NVS_STANDBY_MIN_KEY, &value) == ESP_OK &&
        value >= SETTINGS_STANDBY_MIN_MINUTES && value <= SETTINGS_STANDBY_MAX_MINUTES) {
        s_cfg_standby_minutes = value;
    }
    for (int corner = 0; corner < 4; corner++) {
        char key[12];
        snprintf(key, sizeof(key), "corner%d", corner);
        if (nvs_get_u8(nvs, key, &value) == ESP_OK && value < CW_COUNT) {
            s_corner_widget[corner] = value;
        }
    }
    for (int i = 0; i < THEME_COUNT; i++) {
        char key[12];
        uint32_t color = 0;
        snprintf(key, sizeof(key), "theme%d", i);
        if (nvs_get_u32(nvs, key, &color) == ESP_OK && color <= 0xFFFFFF) {
            s_theme_color[i] = color;
        }
    }
    nvs_close(nvs);
}

static void save_ui_settings(void)
{
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    (void)nvs_set_u8(nvs, SETTINGS_NVS_BRIGHTNESS_KEY, s_cfg_brightness);
    (void)nvs_set_u8(nvs, SETTINGS_NVS_VOLUME_KEY, s_cfg_volume);
    (void)nvs_set_u8(nvs, SETTINGS_NVS_STANDBY_EN_KEY, s_cfg_standby_enabled ? 1 : 0);
    (void)nvs_set_u8(nvs, SETTINGS_NVS_STANDBY_MIN_KEY, s_cfg_standby_minutes);
    for (int corner = 0; corner < 4; corner++) {
        char key[12];
        snprintf(key, sizeof(key), "corner%d", corner);
        (void)nvs_set_u8(nvs, key, s_corner_widget[corner]);
    }
    for (int i = 0; i < THEME_COUNT; i++) {
        char key[12];
        snprintf(key, sizeof(key), "theme%d", i);
        (void)nvs_set_u32(nvs, key, s_theme_color[i]);
    }
    (void)nvs_commit(nvs);
    nvs_close(nvs);
}

static void wake_screen(void)
{
    if (s_screen_off) {
        s_screen_off = false;
        (void)bsp_display_brightness_set(s_cfg_brightness);
    }
}

static void load_wifi_credentials(void)
{
    memset(&s_wifi_credentials, 0, sizeof(s_wifi_credentials));
    copy_string(s_weather_city, sizeof(s_weather_city), WEATHER_CITY);
    copy_string(s_weather_api_key, sizeof(s_weather_api_key), AMAP_WEATHER_KEY);
    copy_string(s_last_weather.city, sizeof(s_last_weather.city), WEATHER_CITY);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_OK) {
        size_t ssid_len = sizeof(s_wifi_credentials.ssid);
        size_t pass_len = sizeof(s_wifi_credentials.password);
        size_t city_len = sizeof(s_weather_city);
        size_t key_len = sizeof(s_weather_api_key);
        esp_err_t ssid_ret =
            nvs_get_str(nvs, WIFI_NVS_SSID_KEY, s_wifi_credentials.ssid, &ssid_len);
        esp_err_t pass_ret =
            nvs_get_str(nvs, WIFI_NVS_PASSWORD_KEY, s_wifi_credentials.password, &pass_len);
        esp_err_t city_ret = nvs_get_str(nvs, WEATHER_NVS_CITY_KEY, s_weather_city, &city_len);
        esp_err_t key_ret =
            nvs_get_str(nvs, WEATHER_NVS_KEY_KEY, s_weather_api_key, &key_len);
        uint32_t saved_goal = 0;
        esp_err_t goal_ret = nvs_get_u32(nvs, PEDOMETER_NVS_GOAL_KEY, &saved_goal);
        if (city_ret == ESP_OK && s_weather_city[0] != '\0') {
            normalize_weather_city(s_weather_city, sizeof(s_weather_city));
            copy_string(s_last_weather.city, sizeof(s_last_weather.city), s_weather_city);
        }
        if (key_ret != ESP_OK || s_weather_api_key[0] == '\0') {
            copy_string(s_weather_api_key, sizeof(s_weather_api_key), AMAP_WEATHER_KEY);
        }
        if (goal_ret == ESP_OK && saved_goal >= PEDOMETER_MIN_GOAL_STEPS &&
            saved_goal <= PEDOMETER_MAX_GOAL_STEPS) {
            s_pedometer_goal_steps = saved_goal;
        }
        nvs_close(nvs);
        ESP_LOGI(TAG, "Weather config from NVS: city=%s key=%s", s_weather_city,
                 weather_api_key_configured() ? "set" : "missing");
        if (ssid_ret == ESP_OK) {
            if (pass_ret != ESP_OK) {
                s_wifi_credentials.password[0] = '\0';
            }
            ESP_LOGI(TAG, "Loaded Wi-Fi credentials from NVS: %s", s_wifi_credentials.ssid);
            return;
        }
    }

    if (config_value_is_set(WIFI_SSID, "YOUR_WIFI_SSID")) {
        copy_string(s_wifi_credentials.ssid, sizeof(s_wifi_credentials.ssid), WIFI_SSID);
        copy_string(s_wifi_credentials.password, sizeof(s_wifi_credentials.password),
                    WIFI_PASSWORD);
        ESP_LOGI(TAG, "Using fallback Wi-Fi credentials from app_config.h: %s",
                 s_wifi_credentials.ssid);
    }
}

static esp_err_t save_pedometer_goal(uint32_t goal_steps)
{
    if (goal_steps < PEDOMETER_MIN_GOAL_STEPS || goal_steps > PEDOMETER_MAX_GOAL_STEPS) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u32(nvs, PEDOMETER_NVS_GOAL_KEY, goal_steps);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        s_pedometer_goal_steps = goal_steps;
        render_pedometer(s_step_count, 0, "Goal saved");
    }
    return ret;
}

static esp_err_t save_weather_city(const char *city)
{
    if (city == NULL || city[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, WEATHER_NVS_CITY_KEY, city);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        copy_string(s_weather_city, sizeof(s_weather_city), city);
        normalize_weather_city(s_weather_city, sizeof(s_weather_city));
        copy_string(s_last_weather.city, sizeof(s_last_weather.city), city);
        normalize_weather_city(s_last_weather.city, sizeof(s_last_weather.city));
    }
    return ret;
}

static esp_err_t save_weather_api_key(const char *api_key)
{
    if (api_key == NULL || api_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, WEATHER_NVS_KEY_KEY, api_key);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        copy_string(s_weather_api_key, sizeof(s_weather_api_key), api_key);
        ESP_LOGI(TAG, "Weather API key saved to NVS");
    }
    return ret;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, WIFI_NVS_SSID_KEY, ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, WIFI_NVS_PASSWORD_KEY, password != NULL ? password : "");
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        copy_string(s_wifi_credentials.ssid, sizeof(s_wifi_credentials.ssid), ssid);
        copy_string(s_wifi_credentials.password, sizeof(s_wifi_credentials.password),
                    password != NULL ? password : "");
    }
    return ret;
}

static const char *page_name(app_page_t page)
{
    switch (page) {
    case APP_PAGE_TIME:
        return "time";
    case APP_PAGE_WEATHER:
        return "weather";
    case APP_PAGE_PEDOMETER:
        return "pedometer";
    case APP_PAGE_CODEX:
        return "codex";
    case APP_PAGE_AGENT:
        return "agent";
    case APP_PAGE_MUSIC:
        return "music";
    default:
        return "unknown";
    }
}

static void get_local_clock(struct tm *out_time)
{
    time_t now;
    time(&now);
    localtime_r(&now, out_time);
}

static void set_default_clock(void)
{
    setenv("TZ", DEFAULT_TIMEZONE, 1);
    tzset();

    struct tm default_time = {
        .tm_year = DEFAULT_TIME_YEAR - 1900,
        .tm_mon = DEFAULT_TIME_MONTH - 1,
        .tm_mday = DEFAULT_TIME_DAY,
        .tm_hour = DEFAULT_TIME_HOUR,
        .tm_min = DEFAULT_TIME_MINUTE,
        .tm_sec = DEFAULT_TIME_SECOND,
        .tm_isdst = -1,
    };
    time_t epoch = mktime(&default_time);
    struct timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    return label;
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }

    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_obj_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == NULL) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_obj_tree_hidden(lv_obj_t *obj, bool hidden)
{
    /* A hidden parent already suppresses its complete subtree. Keeping the
     * child flags untouched lets LVGL invalidate the full-page root once. */
    set_obj_hidden(obj, hidden);
}

static lv_obj_t *create_icon_image(lv_obj_t *parent, const lv_image_dsc_t *source)
{
    lv_obj_t *icon = lv_image_create(parent);
    lv_image_set_src(icon, source);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return icon;
}

static lv_obj_t *create_positioned_icon(lv_obj_t *parent, const lv_image_dsc_t *source,
                                        int32_t x, int32_t y)
{
    lv_obj_t *icon = create_icon_image(parent, source);
    lv_obj_set_pos(icon, x, y);
    return icon;
}

static lv_obj_t *create_main_icon(lv_obj_t *parent, const lv_image_dsc_t *source)
{
    lv_obj_t *icon = create_icon_image(parent, source);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, UI_MAIN_ICON_CENTER_Y);
    return icon;
}

static lv_obj_t *create_metric_icon(lv_obj_t *parent, metric_icon_t type, lv_color_t color)
{
    (void)color;
    lv_obj_t *icon = NULL;
    switch (type) {
    case METRIC_ICON_PIN:
        icon = create_icon_image(parent, &ui_icon_distance);
        break;
    case METRIC_ICON_FLAME:
        icon = create_icon_image(parent, &ui_icon_calories);
        break;
    case METRIC_ICON_HEART:
        icon = create_icon_image(parent, &ui_icon_motion);
        break;
    case METRIC_ICON_USED:
        icon = create_icon_image(parent, &ui_icon_codex_card);
        break;
    case METRIC_ICON_LIMIT:
        icon = create_icon_image(parent, &ui_icon_battery_card);
        break;
    case METRIC_ICON_LEFT:
    default:
        icon = create_icon_image(parent, &ui_icon_battery_card);
        break;
    }
    return icon;
}

static void create_status_bar(lv_obj_t *parent)
{
    s_status_ui.clock_icon = create_positioned_icon(parent, &ui_icon_clock, 118, 36);

    s_status_ui.time_label = create_label(parent, "00:00", FONT_MEDIUM, lv_color_hex(0xF7FBFF));
    lv_obj_set_width(s_status_ui.time_label, 66);
    lv_obj_set_pos(s_status_ui.time_label, 146, 34);

    s_status_ui.separator = lv_obj_create(parent);
    lv_obj_remove_style_all(s_status_ui.separator);
    lv_obj_set_size(s_status_ui.separator, 1, 30);
    lv_obj_set_pos(s_status_ui.separator, 224, 32);
    lv_obj_set_style_bg_color(s_status_ui.separator, lv_color_hex(0x4C5866), 0);
    lv_obj_set_style_bg_opa(s_status_ui.separator, LV_OPA_70, 0);
    lv_obj_clear_flag(s_status_ui.separator, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_status_ui.title_label = create_label(parent, "QMI8658", FONT_MEDIUM, lv_color_hex(0xF7FBFF));
    lv_obj_set_size(s_status_ui.title_label, 90, 24);
    lv_obj_set_style_text_align(s_status_ui.title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_status_ui.title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_status_ui.title_label, 232, 34);

    s_status_ui.battery_icon = create_positioned_icon(parent, &ui_icon_battery, 326, 34);
}

static void set_status_bar_hidden_locked(bool hidden)
{
    set_obj_hidden(s_status_ui.clock_icon, hidden);
    set_obj_hidden(s_status_ui.time_label, hidden);
    set_obj_hidden(s_status_ui.separator, hidden);
    set_obj_hidden(s_status_ui.title_label, hidden);
    set_obj_hidden(s_status_ui.battery_icon, hidden);
}

static void move_overlay_ui_foreground_locked(void)
{
    lv_obj_t *status_objs[] = {
        s_status_ui.clock_icon,
        s_status_ui.time_label,
        s_status_ui.separator,
        s_status_ui.title_label,
        s_status_ui.battery_icon,
        s_dot_time,
        s_dot_weather,
        s_dot_pedometer,
        s_dot_codex,
    };

    for (size_t i = 0; i < sizeof(status_objs) / sizeof(status_objs[0]); i++) {
        if (status_objs[i] != NULL) {
            lv_obj_move_foreground(status_objs[i]);
        }
    }
}

static void update_status_bar_locked(void)
{
    if (s_status_ui.time_label == NULL || s_status_ui.title_label == NULL) {
        return;
    }

    struct tm local_time;
    get_local_clock(&local_time);
    char time_text[8];
    snprintf(time_text, sizeof(time_text), "%02d:%02d", local_time.tm_hour, local_time.tm_min);
    set_label_text_if_changed(s_status_ui.time_label, time_text);

    const char *title = "TIME";
    if (s_current_page == APP_PAGE_WEATHER) {
        title = "WEATHER";
    } else if (s_current_page == APP_PAGE_PEDOMETER) {
        title = "QMI8658";
    } else if (s_current_page == APP_PAGE_CODEX) {
        title = "Codex";
    } else if (s_current_page == APP_PAGE_AGENT) {
        title = "AI";
    } else if (s_current_page == APP_PAGE_MUSIC) {
        title = "MUSIC";
    }
    set_label_text_if_changed(s_status_ui.title_label, title);
}

static void format_step_count(uint32_t value, char *buffer, size_t buffer_size)
{
    char digits[16];
    snprintf(digits, sizeof(digits), "%lu", (unsigned long)value);

    size_t len = strlen(digits);
    size_t commas = len > 0 ? (len - 1U) / 3U : 0U;
    size_t out_len = len + commas;
    if (buffer_size <= out_len) {
        snprintf(buffer, buffer_size, "%lu", (unsigned long)value);
        return;
    }

    buffer[out_len] = '\0';
    size_t src = len;
    size_t dst = out_len;
    uint32_t group = 0;
    while (src > 0) {
        buffer[--dst] = digits[--src];
        group++;
        if (group == 3 && src > 0) {
            buffer[--dst] = ',';
            group = 0;
        }
    }
}

static lv_obj_t *create_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(page, page_nav_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(page, page_nav_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(page, page_nav_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(page, page_nav_event_cb, LV_EVENT_PRESS_LOST, NULL);
    return page;
}

static lv_obj_t *create_metric_column(lv_obj_t *parent, int32_t x_ofs, bool with_icon,
                                      metric_icon_t icon_type, const char *label_text,
                                      const char *initial_value)
{
    if (with_icon) {
        lv_obj_t *icon = create_metric_icon(parent, icon_type, lv_color_hex(0xFFFFFF));
        lv_obj_align(icon, LV_ALIGN_CENTER, x_ofs, UI_METRIC_ICON_CENTER_Y);
    }

    lv_obj_t *value_label = create_label(parent, initial_value, FONT_TITLE,
                                         lv_color_hex(0xF4FAFF));
    lv_obj_align(value_label, LV_ALIGN_CENTER, x_ofs, UI_METRIC_VALUE_CENTER_Y);

    lv_obj_t *title_label = create_label(parent, label_text, FONT_SMALL,
                                         lv_color_hex(0x9AA7B5));
    lv_obj_align(title_label, LV_ALIGN_CENTER, x_ofs, UI_METRIC_LABEL_CENTER_Y);

    return value_label;
}

static void update_page_dots_locked(void)
{
    if (s_dot_time == NULL || s_dot_weather == NULL ||
        s_dot_pedometer == NULL || s_dot_codex == NULL) {
        return;
    }

    if (!PAGE_DOTS_VISIBLE) {
        set_obj_hidden(s_dot_time, true);
        set_obj_hidden(s_dot_weather, true);
        set_obj_hidden(s_dot_pedometer, true);
        set_obj_hidden(s_dot_codex, true);
        return;
    }

    set_obj_hidden(s_dot_time, false);
    set_obj_hidden(s_dot_weather, false);
    set_obj_hidden(s_dot_pedometer, false);
    set_obj_hidden(s_dot_codex, false);

    bool time_active = s_current_page == APP_PAGE_TIME;
    bool weather_active = s_current_page == APP_PAGE_WEATHER;
    bool pedometer_active = s_current_page == APP_PAGE_PEDOMETER;
    bool codex_active = s_current_page == APP_PAGE_CODEX;

    lv_obj_set_size(s_dot_time, time_active ? 22 : 5, 5);
    lv_obj_set_size(s_dot_weather, weather_active ? 22 : 5, 5);
    lv_obj_set_size(s_dot_pedometer, pedometer_active ? 22 : 5, 5);
    lv_obj_set_size(s_dot_codex, codex_active ? 22 : 5, 5);
    lv_obj_set_style_bg_color(s_dot_time,
                              lv_color_hex(time_active ? 0xF7FBFF : 0x32404E), 0);
    lv_obj_set_style_bg_color(s_dot_weather,
                              lv_color_hex(weather_active ? 0xFFD166 : 0x32404E), 0);
    lv_obj_set_style_bg_color(s_dot_pedometer,
                              lv_color_hex(pedometer_active ? 0x18D7F5 : 0x32404E), 0);
    lv_obj_set_style_bg_color(s_dot_codex,
                              lv_color_hex(codex_active ? 0xFFD166 : 0x32404E), 0);
    lv_obj_align(s_dot_time, LV_ALIGN_BOTTOM_MID, -48, -8);
    lv_obj_align(s_dot_weather, LV_ALIGN_BOTTOM_MID, -16, -8);
    lv_obj_align(s_dot_pedometer, LV_ALIGN_BOTTOM_MID, 16, -8);
    lv_obj_align(s_dot_codex, LV_ALIGN_BOTTOM_MID, 48, -8);
}

static void enforce_page_visibility_locked(void)
{
    bool time_active = s_current_page == APP_PAGE_TIME;
    bool weather_active = s_current_page == APP_PAGE_WEATHER;
    bool pedometer_active = s_current_page == APP_PAGE_PEDOMETER;
    bool codex_active = s_current_page == APP_PAGE_CODEX;
    bool agent_active = s_current_page == APP_PAGE_AGENT;
    bool music_active = s_current_page == APP_PAGE_MUSIC;

    set_obj_tree_hidden(s_time_ui.page, !time_active);
    set_obj_tree_hidden(s_weather_ui.page, !weather_active);
    set_obj_tree_hidden(s_pedometer_ui.page, !pedometer_active);
    set_obj_tree_hidden(s_codex_ui.page, !codex_active);
    set_obj_tree_hidden(s_agent_ui.page, !agent_active);
    set_obj_tree_hidden(s_music_ui.page, !music_active);

    if (time_active && s_time_ui.page != NULL) {
        lv_obj_move_foreground(s_time_ui.page);
    } else if (weather_active && s_weather_ui.page != NULL) {
        lv_obj_move_foreground(s_weather_ui.page);
    } else if (pedometer_active && s_pedometer_ui.page != NULL) {
        lv_obj_move_foreground(s_pedometer_ui.page);
    } else if (codex_active && s_codex_ui.page != NULL) {
        lv_obj_move_foreground(s_codex_ui.page);
    } else if (agent_active && s_agent_ui.page != NULL) {
        lv_obj_move_foreground(s_agent_ui.page);
    } else if (music_active && s_music_ui.page != NULL) {
        lv_obj_move_foreground(s_music_ui.page);
    }

    move_overlay_ui_foreground_locked();
    update_page_dots_locked();
    set_status_bar_hidden_locked(true);
    agent_refresh_lamp();
}

static app_page_t relative_page(app_page_t base, int delta)
{
    int page = (int)base + delta;
    while (page < 0) {
        page += APP_PAGE_COUNT;
    }
    return (app_page_t)(page % APP_PAGE_COUNT);
}

static void set_active_page_locked(app_page_t page)
{
    /* The forecast overlay belongs to the weather page only; leaving that
     * page must not strand it on top of another one. */
    if (s_forecast_open && page != APP_PAGE_WEATHER) {
        s_forecast_open = false;
        set_obj_hidden(s_forecast_ui.page, true);
    }
    s_current_page = page;
    enforce_page_visibility_locked();
    if (page == APP_PAGE_TIME) {
        update_time_page_locked();
    }
    update_status_bar_locked();
    /* Page arcs touch the round bezel. Redraw the complete screen so their
     * antialiased edge pixels cannot survive after the page is hidden. */
    lv_obj_invalidate(lv_screen_active());
}

/*
 * Page transitions cross-fade via the backlight PWM instead of moving pixels:
 * the 14-row draw buffer + PSRAM dial image + no-TE panel make a full-screen
 * slide tear and stutter, whereas dimming the backlight to black, switching
 * the page in that dark instant, and brightening again costs zero rendering
 * and cannot tear. It also matches the "old fades out, new fades in" request.
 */
static uint8_t s_page_fade_var;
static app_page_t s_page_fade_target;

static void page_fade_exec_cb(void *var, int32_t value)
{
    (void)var;
    if (!s_screen_off) {
        (void)bsp_display_brightness_set((int)value);
    }
}

static void page_fade_up(void)
{
    lv_anim_t up;
    lv_anim_init(&up);
    lv_anim_set_var(&up, &s_page_fade_var);
    lv_anim_set_values(&up, 0, s_cfg_brightness);
    lv_anim_set_duration(&up, PAGE_ANIM_MS / 2);
    lv_anim_set_exec_cb(&up, page_fade_exec_cb);
    lv_anim_set_path_cb(&up, lv_anim_path_ease_out);
    lv_anim_start(&up);
}

/* Reached full black: swap the page (invisible) then fade back up. */
static void page_fade_dark_ready_cb(lv_anim_t *anim)
{
    (void)anim;
    set_active_page_locked(s_page_fade_target);
    page_fade_up();
}

static void set_active_page_animated(app_page_t page, int dir)
{
    (void)dir;
    if (page == s_current_page) {
        return;
    }
    if (s_screen_off) {
        set_active_page_locked(page);
        return;
    }

    lv_anim_delete(&s_page_fade_var, page_fade_exec_cb);
    s_page_fade_target = page;

    lv_anim_t down;
    lv_anim_init(&down);
    lv_anim_set_var(&down, &s_page_fade_var);
    lv_anim_set_values(&down, s_cfg_brightness, 0);
    lv_anim_set_duration(&down, PAGE_ANIM_MS / 2);
    lv_anim_set_exec_cb(&down, page_fade_exec_cb);
    lv_anim_set_path_cb(&down, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&down, page_fade_dark_ready_cb);
    lv_anim_start(&down);
}

static void set_relative_page_locked(int delta)
{
    uint32_t current_ms = now_ms();
    if ((current_ms - s_last_page_nav_ms) >= PAGE_NAV_DEBOUNCE_MS) {
        s_last_page_nav_ms = current_ms;
        set_active_page_animated(relative_page(s_current_page, delta), delta);
    }
}

static void page_nav_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL) {
        indev = lv_indev_active();
    }

    if (code == LV_EVENT_PRESSED) {
        s_last_activity_ms = now_ms();
        if (s_screen_off && indev != NULL) {
            /* First touch only wakes the screen; swallow it. */
            wake_screen();
            s_page_press_active = false;
            return;
        }
        if (indev == NULL) {
            s_page_press_active = false;
            return;
        }
        lv_indev_get_point(indev, &s_page_press_point);
        s_page_last_point = s_page_press_point;
        s_page_press_active = true;
        s_page_press_start_ms = now_ms();
        return;
    }

    if (!s_page_press_active) {
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (indev == NULL) {
            return;
        }

        lv_indev_get_point(indev, &s_page_last_point);
        return;
    }

    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }

    if (code == LV_EVENT_RELEASED && indev != NULL) {
        lv_indev_get_point(indev, &s_page_last_point);
    }
    lv_point_t release_point = s_page_last_point;
    s_page_press_active = false;

    int dx = (int)release_point.x - (int)s_page_press_point.x;
    int dy = (int)release_point.y - (int)s_page_press_point.y;
    int abs_dx = abs(dx);
    int abs_dy = abs(dy);

    bool horizontal_commit = abs_dx >= PAGE_SWIPE_MIN_PX && abs_dx > abs_dy &&
                             abs_dy <= PAGE_SWIPE_MAX_OFF_AXIS_PX;

    /* While the forecast overlay is up it owns horizontal swipes (day
     * stepping) and taps (dismiss); everything else stays untouched. */
    if (s_forecast_open) {
        if (horizontal_commit) {
            int count = s_last_weather.day_count > 0 ? s_last_weather.day_count : 1;
            s_forecast_day = (s_forecast_day + (dx < 0 ? 1 : count - 1)) % count;
            render_forecast_locked();
        } else if (code == LV_EVENT_RELEASED && abs_dx < PAGE_SWIPE_MIN_PX &&
                   abs_dy < PAGE_SWIPE_MIN_PX) {
            show_forecast_page(false);
        }
        return;
    }

    if (horizontal_commit) {
        set_relative_page_locked(dx < 0 ? 1 : -1);
    } else if (dy >= PAGE_SWIPE_MIN_PX && abs_dy > abs_dx &&
               abs_dx <= PAGE_SWIPE_MAX_OFF_AXIS_PX) {
        /* Swipe down: open the settings panel, phone style. */
        show_settings_panel(true);
    } else if (code == LV_EVENT_RELEASED && abs_dx < PAGE_SWIPE_MIN_PX &&
               abs_dy < PAGE_SWIPE_MIN_PX) {
        uint32_t press_elapsed_ms = now_ms() - s_page_press_start_ms;
        if (press_elapsed_ms >= PAGE_LONG_PRESS_MS) {
            if (s_current_page == APP_PAGE_TIME) {
                s_ntp_resync_requested = true;
            } else if (s_current_page == APP_PAGE_WEATHER) {
                request_weather_refresh();
            } else if (s_current_page == APP_PAGE_CODEX ||
                       s_current_page == APP_PAGE_AGENT) {
                request_codex_refresh();
            }
        } else if (s_current_page == APP_PAGE_TIME && corner_handle_tap(release_point)) {
            /* Tap on a watch-face corner cycles that complication instead of
             * switching pages. */
        } else if (s_current_page == APP_PAGE_WEATHER &&
                   weather_icon_hit(release_point)) {
            show_forecast_page(true);
        } else {
            set_relative_page_locked(1);
        }
    }
}

static void draw_canvas_text(lv_layer_t *layer, const char *text, const lv_font_t *font,
                             lv_color_t color, int32_t cx, int32_t cy)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = text;
    dsc.text_local = 1;
    dsc.font = font;
    dsc.color = color;
    dsc.align = LV_TEXT_ALIGN_CENTER;

    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_area_t coords = {
        cx - size.x / 2,
        cy - size.y / 2,
        cx - size.x / 2 + size.x - 1,
        cy - size.y / 2 + size.y - 1,
    };
    lv_draw_label(layer, &dsc, &coords);
}

/* Static dial artwork (ticks, numerals, corner captions) is rendered once
 * into a PSRAM canvas so per-frame redraws are a plain image blit. */
static lv_obj_t *create_dial_canvas(lv_obj_t *parent)
{
    size_t buf_size = (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * 2;
    void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Dial canvas alloc failed");
        return NULL;
    }

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buf, BSP_LCD_H_RES, BSP_LCD_V_RES, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    for (int i = 0; i < 60; i++) {
        bool major = (i % 5) == 0;
        float a = WATCH_DEG_TO_RAD(i * 6 - 90);
        float ca = cosf(a);
        float sa = sinf(a);
        int32_t inner_r = major ? WATCH_TICK_MAJOR_R : WATCH_TICK_MINOR_R;

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(major ? WATCH_COLOR_TICK_MAJOR : WATCH_COLOR_TICK_MINOR);
        dsc.width = major ? 4 : 2;
        dsc.round_start = 1;
        dsc.round_end = 1;
        dsc.p1.x = WATCH_CENTER_X + ca * WATCH_TICK_OUTER_R;
        dsc.p1.y = WATCH_CENTER_Y + sa * WATCH_TICK_OUTER_R;
        dsc.p2.x = WATCH_CENTER_X + ca * inner_r;
        dsc.p2.y = WATCH_CENTER_Y + sa * inner_r;
        lv_draw_line(&layer, &dsc);
    }

    lv_color_t numeral_color = lv_color_hex(WATCH_COLOR_TICK_MAJOR);
    draw_canvas_text(&layer, "12", FONT_VALUE, numeral_color, WATCH_CENTER_X,
                     WATCH_CENTER_Y - WATCH_NUMERAL_R);
    draw_canvas_text(&layer, "3", FONT_VALUE, numeral_color, WATCH_CENTER_X + WATCH_NUMERAL_R,
                     WATCH_CENTER_Y);
    draw_canvas_text(&layer, "6", FONT_VALUE, numeral_color, WATCH_CENTER_X,
                     WATCH_CENTER_Y + WATCH_NUMERAL_R);
    draw_canvas_text(&layer, "9", FONT_VALUE, numeral_color, WATCH_CENTER_X - WATCH_NUMERAL_R,
                     WATCH_CENTER_Y);

    /* Corner captions are live labels now that the corners are tap-cyclable
     * widgets, so the canvas only carries the ticks and numerals. */
    lv_canvas_finish_layer(canvas, &layer);
    return canvas;
}

static lv_obj_t *create_corner_gauge(lv_obj_t *parent, int32_t x_ofs, int32_t y_ofs,
                                     uint32_t accent, int32_t min_value, int32_t max_value)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, WATCH_GAUGE_SIZE, WATCH_GAUGE_SIZE);
    lv_obj_align(arc, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_arc_set_range(arc, min_value, max_value);
    lv_arc_set_value(arc, min_value);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(WATCH_COLOR_GAUGE_BG), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(accent), LV_PART_INDICATOR);
    return arc;
}

static const char *weekday_short_cn(int wday)
{
    static const char *const days[] = {"日", "一", "二", "三", "四", "五", "六"};
    if (wday < 0 || wday > 6) {
        return "-";
    }
    return days[wday];
}

static int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 31;
    }
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return days[month - 1];
}

static int32_t corner_x_ofs(int corner)
{
    return (corner == 0 || corner == 2) ? -WATCH_CORNER_OFS : WATCH_CORNER_OFS;
}

static int32_t corner_y_ofs(int corner)
{
    return corner < 2 ? -WATCH_CORNER_OFS : WATCH_CORNER_OFS;
}

/* Configure one corner's shared objects (gauge + value + caption) for the
 * widget type it currently shows. Values are filled by
 * corner_render_widget_locked each second. */
static void corner_apply_widget_locked(int corner)
{
    lv_obj_t *arc = s_time_ui.corner_arc[corner];
    lv_obj_t *value = s_time_ui.corner_value[corner];
    lv_obj_t *caption = s_time_ui.corner_caption[corner];
    if (arc == NULL || value == NULL || caption == NULL) {
        return;
    }

    corner_widget_t type = (corner_widget_t)s_corner_widget[corner];
    int32_t cx = corner_x_ofs(corner);
    int32_t cy = corner_y_ofs(corner);
    /* Top corners put the caption under the content, bottom corners above. */
    int32_t caption_y = corner < 2 ? cy + 45 : cy - 45;

    set_obj_hidden(arc, false);

    uint32_t arc_color = s_theme_color[THEME_BATTERY];
    int32_t range_min = 0;
    int32_t range_max = 100;
    const lv_font_t *value_font = FONT_SMALL;
    uint32_t value_color = 0xFFFFFF;
    int32_t value_y = cy;
    const char *caption_text = "";
    uint32_t caption_color = WATCH_COLOR_CAPTION;

    switch (type) {
    case CW_BATTERY:
        caption_text = "电量";
        break;
    case CW_TEMP:
        arc_color = s_theme_color[THEME_TEMP];
        range_min = -10;
        range_max = 50;
        caption_text = "温度";
        break;
    case CW_WEEKDAY:
        /* Ring fills wday/7; the single weekday character sits inside. */
        arc_color = s_theme_color[THEME_WEEKDAY];
        range_max = 7;
        value_font = FONT_CJK;
        value_color = s_theme_color[THEME_WEEKDAY];
        caption_text = "星期";
        break;
    case CW_DATE:
        /* Ring fills day/days-in-month; caption doubles as the month. */
        arc_color = s_theme_color[THEME_DATE];
        range_max = 31;
        value_font = FONT_MEDIUM;
        caption_color = s_theme_color[THEME_DATE];
        break;
    case CW_WEATHER:
        /* Ring shows where the live temperature sits inside today's
         * forecast low..high; range is set per render. */
        arc_color = s_theme_color[THEME_WEATHER];
        value_font = FONT_MEDIUM;
        caption_text = "天气";
        caption_color = WATCH_COLOR_WEEKDAY;
        break;
    case CW_CODEX:
        arc_color = s_theme_color[THEME_CODEX];
        caption_text = "Codex";
        break;
    case CW_AGENT:
        caption_text = "AI";
        break;
    default:
        break;
    }

    lv_arc_set_range(arc, range_min, range_max);
    lv_arc_set_value(arc, range_min);
    lv_obj_set_style_arc_color(arc, lv_color_hex(arc_color), LV_PART_INDICATOR);

    lv_obj_set_style_text_font(value, value_font, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(value_color), 0);
    lv_obj_align(value, LV_ALIGN_CENTER, cx, value_y);
    lv_label_set_text(value, "--");

    lv_obj_set_style_text_color(caption, lv_color_hex(caption_color), 0);
    lv_obj_align(caption, LV_ALIGN_CENTER, cx, caption_y);
    lv_label_set_text(caption, caption_text);
}

static void corner_render_widget_locked(int corner, const struct tm *local_time)
{
    lv_obj_t *arc = s_time_ui.corner_arc[corner];
    lv_obj_t *value = s_time_ui.corner_value[corner];
    lv_obj_t *caption = s_time_ui.corner_caption[corner];
    if (arc == NULL || value == NULL || caption == NULL) {
        return;
    }

    char text[24];
    switch ((corner_widget_t)s_corner_widget[corner]) {
    case CW_BATTERY: {
        int battery = s_battery_percent;
        if (battery >= 0) {
            snprintf(text, sizeof(text), "%d%%", battery);
            if (lv_arc_get_value(arc) != battery) {
                lv_arc_set_value(arc, battery);
            }
        } else {
            snprintf(text, sizeof(text), "--");
        }
        set_label_text_if_changed(value, text);
        break;
    }
    case CW_TEMP: {
        float temp_c = s_board_temp_c;
        if (temp_c > -100.0f) {
            int temp_rounded = (int)lroundf(temp_c);
            snprintf(text, sizeof(text), "%d°", temp_rounded);
            if (lv_arc_get_value(arc) != temp_rounded) {
                lv_arc_set_value(arc, temp_rounded);
            }
        } else {
            snprintf(text, sizeof(text), "--");
        }
        set_label_text_if_changed(value, text);
        break;
    }
    case CW_WEEKDAY: {
        /* Monday=1 .. Sunday=7, so the ring reads as week progress. */
        int wday7 = local_time->tm_wday == 0 ? 7 : local_time->tm_wday;
        if (lv_arc_get_value(arc) != wday7) {
            lv_arc_set_value(arc, wday7);
        }
        set_label_text_if_changed(value, weekday_short_cn(local_time->tm_wday));
        break;
    }
    case CW_DATE: {
        int days = days_in_month(local_time->tm_year + 1900, local_time->tm_mon + 1);
        if (lv_arc_get_max_value(arc) != days) {
            lv_arc_set_range(arc, 0, days);
        }
        if (lv_arc_get_value(arc) != local_time->tm_mday) {
            lv_arc_set_value(arc, local_time->tm_mday);
        }
        snprintf(text, sizeof(text), "%d月", local_time->tm_mon + 1);
        set_label_text_if_changed(caption, text);
        snprintf(text, sizeof(text), "%d", local_time->tm_mday);
        set_label_text_if_changed(value, text);
        break;
    }
    case CW_WEATHER:
        if (s_last_weather.valid) {
            int low = s_last_weather.min_temp_c;
            int high = s_last_weather.max_temp_c;
            if (high <= low) {
                high = low + 1;
            }
            if (lv_arc_get_min_value(arc) != low || lv_arc_get_max_value(arc) != high) {
                lv_arc_set_range(arc, low, high);
            }
            int current = clamp_int(s_last_weather.current_temp_c, low, high);
            if (lv_arc_get_value(arc) != current) {
                lv_arc_set_value(arc, current);
            }
            snprintf(text, sizeof(text), "%d°", s_last_weather.current_temp_c);
            set_label_text_if_changed(value, text);
            const char *cond = text_glyphs_available(FONT_CJK, s_last_weather.condition)
                                   ? s_last_weather.condition
                                   : weather_condition_en(s_last_weather.weather_code);
            set_label_text_if_changed(caption, cond);
        } else {
            set_label_text_if_changed(value, "--");
            set_label_text_if_changed(caption, "天气");
        }
        break;
    case CW_CODEX: {
        uint64_t limit = s_last_codex_usage.limit_tokens;
        if (limit > 0) {
            int percent = (int)clamp_u32(
                (uint32_t)((s_last_codex_usage.used_tokens * 100ULL) / limit), 100U);
            snprintf(text, sizeof(text), "%d%%", percent);
            if (lv_arc_get_value(arc) != percent) {
                lv_arc_set_value(arc, percent);
            }
        } else {
            snprintf(text, sizeof(text), "--");
        }
        set_label_text_if_changed(value, text);
        break;
    }
    case CW_AGENT: {
        agent_state_t state = s_agent_status.valid ? s_agent_status.state
                                                   : AGENT_STATE_UNKNOWN;
        lv_obj_set_style_arc_color(arc, lv_color_hex(agent_state_color(state)),
                                   LV_PART_INDICATOR);
        if (lv_arc_get_value(arc) != 100) {
            lv_arc_set_value(arc, 100);
        }
        set_label_text_if_changed(value, agent_state_short(state));
        break;
    }
    default:
        break;
    }
}

/* Push the (possibly just web-edited) theme colors onto every ring that is
 * already built. Corner arcs are handled by re-applying their widget type. */
static void apply_theme_colors(void)
{
    if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }
    for (int corner = 0; corner < 4; corner++) {
        corner_apply_widget_locked(corner);
        struct tm local_time;
        get_local_clock(&local_time);
        corner_render_widget_locked(corner, &local_time);
    }
    if (s_pedometer_ui.progress_arc != NULL) {
        lv_obj_set_style_arc_color(s_pedometer_ui.progress_arc,
                                   lv_color_hex(s_theme_color[THEME_STEPS]),
                                   LV_PART_INDICATOR);
    }
    if (s_codex_ui.usage_arc != NULL) {
        lv_obj_set_style_arc_color(s_codex_ui.usage_arc,
                                   lv_color_hex(s_theme_color[THEME_CODEX]),
                                   LV_PART_INDICATOR);
    }
    if (s_music_ui.vu_arc != NULL) {
        lv_obj_set_style_arc_color(s_music_ui.vu_arc,
                                   lv_color_hex(s_theme_color[THEME_MUSIC]),
                                   LV_PART_INDICATOR);
    }
    bsp_display_unlock();
}

/* A short tap on the clock face cycles the corner under the finger to the
 * next widget type; returns false when the tap missed every corner. */
static bool corner_handle_tap(lv_point_t point)
{
    for (int corner = 0; corner < 4; corner++) {
        int32_t dx = point.x - (WATCH_CENTER_X + corner_x_ofs(corner));
        int32_t dy = point.y - (WATCH_CENTER_Y + corner_y_ofs(corner));
        if (dx * dx + dy * dy <= 58 * 58) {
            s_corner_widget[corner] = (s_corner_widget[corner] + 1) % CW_COUNT;
            corner_apply_widget_locked(corner);
            struct tm local_time;
            get_local_clock(&local_time);
            corner_render_widget_locked(corner, &local_time);
            save_ui_settings();
            return true;
        }
    }
    return false;
}

static lv_obj_t *create_watch_hand(lv_obj_t *parent, int32_t width, uint32_t color,
                                   lv_point_precise_t *points)
{
    points[0].x = WATCH_CENTER_X;
    points[0].y = WATCH_CENTER_Y;
    points[1].x = WATCH_CENTER_X;
    points[1].y = WATCH_CENTER_Y;

    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, points, 2);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

/* Keep the line object's bounding box tight around the hand so a sweeping
 * second hand only invalidates the strip it actually covers. */
static void set_hand_angle(lv_obj_t *hand, lv_point_precise_t *points, float angle_deg,
                           float length, float tail, int32_t width)
{
    float a = WATCH_DEG_TO_RAD(angle_deg - 90.0f);
    float ca = cosf(a);
    float sa = sinf(a);
    float x0 = (float)WATCH_CENTER_X - ca * tail;
    float y0 = (float)WATCH_CENTER_Y - sa * tail;
    float x1 = (float)WATCH_CENTER_X + ca * length;
    float y1 = (float)WATCH_CENTER_Y + sa * length;
    float margin = (float)width;
    float min_x = (x0 < x1 ? x0 : x1) - margin;
    float min_y = (y0 < y1 ? y0 : y1) - margin;

    points[0].x = (lv_value_precise_t)(x0 - min_x);
    points[0].y = (lv_value_precise_t)(y0 - min_y);
    points[1].x = (lv_value_precise_t)(x1 - min_x);
    points[1].y = (lv_value_precise_t)(y1 - min_y);
    lv_obj_set_pos(hand, (int32_t)min_x, (int32_t)min_y);
    lv_line_set_points(hand, points, 2);
}

static void create_time_page(lv_obj_t *parent)
{
    s_time_ui.page = create_page(parent);
    lv_obj_set_style_bg_color(s_time_ui.page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_time_ui.page, LV_OPA_COVER, 0);

    create_dial_canvas(s_time_ui.page);

    for (int corner = 0; corner < 4; corner++) {
        s_time_ui.corner_arc[corner] =
            create_corner_gauge(s_time_ui.page, corner_x_ofs(corner), corner_y_ofs(corner),
                                WATCH_COLOR_BATTERY, 0, 100);
        s_time_ui.corner_value[corner] = create_label(s_time_ui.page, "--", FONT_SMALL,
                                                      lv_color_hex(0xFFFFFF));
        s_time_ui.corner_caption[corner] = create_label(s_time_ui.page, "", FONT_CJK,
                                                        lv_color_hex(WATCH_COLOR_CAPTION));
        corner_apply_widget_locked(corner);
    }

    s_time_ui.digital_label = create_label(s_time_ui.page, "00:00:00", FONT_TITLE,
                                           lv_color_hex(WATCH_COLOR_DIGITAL));
    lv_obj_align(s_time_ui.digital_label, LV_ALIGN_CENTER, 0, 69);

    s_time_ui.status_label = create_label(s_time_ui.page, "AP 192.168.4.1", FONT_SMALL,
                                          lv_color_hex(WATCH_COLOR_BATTERY));
    lv_obj_align(s_time_ui.status_label, LV_ALIGN_CENTER, 0, 96);

    s_time_ui.wifi_icon = create_label(s_time_ui.page, LV_SYMBOL_WIFI, FONT_MEDIUM,
                                       lv_color_hex(WATCH_COLOR_CAPTION));
    lv_obj_align(s_time_ui.wifi_icon, LV_ALIGN_CENTER, -18, 97);

    s_time_ui.ntp_icon = create_label(s_time_ui.page, LV_SYMBOL_REFRESH, FONT_MEDIUM,
                                      lv_color_hex(WATCH_COLOR_CAPTION));
    lv_obj_align(s_time_ui.ntp_icon, LV_ALIGN_CENTER, 18, 97);

    s_time_ui.hour_hand = create_watch_hand(s_time_ui.page, 9, WATCH_COLOR_HAND,
                                            s_hour_hand_points);
    s_time_ui.minute_hand = create_watch_hand(s_time_ui.page, 6, WATCH_COLOR_HAND,
                                              s_minute_hand_points);
    s_time_ui.second_hand = create_watch_hand(s_time_ui.page, 3, WATCH_COLOR_SECOND,
                                              s_second_hand_points);

    lv_obj_t *cap = lv_obj_create(s_time_ui.page);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 18, 18);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(WATCH_COLOR_HAND), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cap_inner = lv_obj_create(s_time_ui.page);
    lv_obj_remove_style_all(cap_inner);
    lv_obj_set_size(cap_inner, 8, 8);
    lv_obj_align(cap_inner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(cap_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap_inner, lv_color_hex(WATCH_COLOR_SECOND), 0);
    lv_obj_set_style_bg_opa(cap_inner, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cap_inner, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

#define WEATHER_ICON_SIZE 96

static void icon_draw_circle(lv_layer_t *layer, uint32_t color, int32_t cx, int32_t cy,
                             int32_t r)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;
    lv_area_t area = { cx - r, cy - r, cx + r - 1, cy + r - 1 };
    lv_draw_rect(layer, &dsc, &area);
}

static void icon_draw_round_rect(lv_layer_t *layer, uint32_t color, int32_t x1, int32_t y1,
                                 int32_t x2, int32_t y2, int32_t radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    lv_area_t area = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &dsc, &area);
}

static void icon_draw_line(lv_layer_t *layer, uint32_t color, int32_t width, float x1,
                           float y1, float x2, float y2)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.width = width;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

static void icon_draw_cloud(lv_layer_t *layer, uint32_t color, int32_t cx, int32_t cy)
{
    icon_draw_circle(layer, color, cx - 14, cy + 2, 11);
    icon_draw_circle(layer, color, cx + 2, cy - 6, 15);
    icon_draw_circle(layer, color, cx + 16, cy + 4, 10);
    icon_draw_round_rect(layer, color, cx - 22, cy + 2, cx + 24, cy + 13, 6);
}

static void icon_draw_sun(lv_layer_t *layer, uint32_t color, int32_t cx, int32_t cy,
                          int32_t r, int32_t ray_inner, int32_t ray_outer)
{
    icon_draw_circle(layer, color, cx, cy, r);
    for (int i = 0; i < 8; i++) {
        float a = WATCH_DEG_TO_RAD(i * 45);
        float ca = cosf(a);
        float sa = sinf(a);
        icon_draw_line(layer, color, 4, (float)cx + ca * ray_inner, (float)cy + sa * ray_inner,
                       (float)cx + ca * ray_outer, (float)cy + sa * ray_outer);
    }
}

/* Weather condition icons drawn as flat vector art on a small canvas. */
static void draw_weather_icon(lv_obj_t *canvas, int code)
{
    if (canvas == NULL) {
        return;
    }

    lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    if (code == 0) {
        /* Clear sky */
        icon_draw_sun(&layer, 0xFFD60A, 48, 48, 18, 25, 33);
    } else if (code == 2) {
        /* Partly cloudy: sun peeking behind a cloud */
        icon_draw_sun(&layer, 0xFFD60A, 34, 32, 13, 18, 25);
        icon_draw_cloud(&layer, 0xE5E5EA, 52, 58);
    } else if (code == 3) {
        /* Overcast */
        icon_draw_cloud(&layer, 0x9AA7B5, 48, 50);
    } else if (code == 45 || code == 48) {
        /* Fog / haze */
        icon_draw_line(&layer, 0x9AA7B5, 5, 22, 32, 74, 32);
        icon_draw_line(&layer, 0x9AA7B5, 5, 14, 48, 66, 48);
        icon_draw_line(&layer, 0x9AA7B5, 5, 26, 64, 78, 64);
    } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        /* Rain */
        icon_draw_cloud(&layer, 0xC9D3DC, 48, 38);
        icon_draw_line(&layer, 0x18D7F5, 4, 34, 62, 28, 78);
        icon_draw_line(&layer, 0x18D7F5, 4, 50, 62, 44, 78);
        icon_draw_line(&layer, 0x18D7F5, 4, 66, 62, 60, 78);
    } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        /* Snow */
        icon_draw_cloud(&layer, 0xC9D3DC, 48, 38);
        icon_draw_circle(&layer, 0xF7FBFF, 34, 70, 4);
        icon_draw_circle(&layer, 0xF7FBFF, 50, 76, 4);
        icon_draw_circle(&layer, 0xF7FBFF, 66, 70, 4);
    } else if (code >= 95) {
        /* Thunderstorm */
        icon_draw_cloud(&layer, 0x8E99A5, 48, 36);
        icon_draw_line(&layer, 0xFFD60A, 5, 54, 54, 43, 71);
        icon_draw_line(&layer, 0xFFD60A, 5, 43, 71, 53, 71);
        icon_draw_line(&layer, 0xFFD60A, 5, 53, 71, 41, 90);
    } else {
        /* Unknown / waiting for data */
        icon_draw_cloud(&layer, 0x5F6B77, 48, 50);
    }

    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_invalidate(canvas);
}

/* The weather-page icon is the entry point to the forecast detail; match
 * the canvas placement in create_weather_page. */
static bool weather_icon_hit(lv_point_t point)
{
    int32_t dx = point.x - WATCH_CENTER_X;
    int32_t dy = point.y - (WATCH_CENTER_Y - 124);
    return dx * dx + dy * dy <= 62 * 62;
}

static lv_obj_t *create_weather_icon_visual(lv_obj_t *parent)
{
    size_t buf_size = (size_t)WEATHER_ICON_SIZE * WEATHER_ICON_SIZE * 2;
    void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Weather icon canvas alloc failed");
        return NULL;
    }

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buf, WEATHER_ICON_SIZE, WEATHER_ICON_SIZE,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, -124);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    draw_weather_icon(canvas, -1);
    return canvas;
}

static void create_weather_page(lv_obj_t *parent)
{
    s_weather_ui.page = create_page(parent);

    s_weather_ui.temp_arc = lv_arc_create(s_weather_ui.page);
    lv_obj_set_size(s_weather_ui.temp_arc, UI_ARC_SIZE, UI_ARC_SIZE);
    lv_obj_align(s_weather_ui.temp_arc, LV_ALIGN_TOP_MID, 0, UI_ARC_TOP);
    lv_arc_set_range(s_weather_ui.temp_arc, -30, 45);
    lv_arc_set_value(s_weather_ui.temp_arc, 0);
    lv_arc_set_rotation(s_weather_ui.temp_arc, 135);
    lv_arc_set_bg_angles(s_weather_ui.temp_arc, 0, 270);
    lv_obj_remove_style(s_weather_ui.temp_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_weather_ui.temp_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_weather_ui.temp_arc, UI_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_weather_ui.temp_arc, UI_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_weather_ui.temp_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_weather_ui.temp_arc, lv_color_hex(0x17212B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_weather_ui.temp_arc, lv_color_hex(0xFFD166),
                               LV_PART_INDICATOR);

    /* City sits below the condition text so the icon has the top to itself. */
    s_weather_ui.city_label = create_label(s_weather_ui.page, "Qingdao", FONT_CJK,
                                           lv_color_hex(0x8DDFFF));
    lv_obj_set_width(s_weather_ui.city_label, 280);
    lv_obj_set_style_text_align(s_weather_ui.city_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_weather_ui.city_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_weather_ui.city_label, LV_ALIGN_CENTER, 0, 80);

    s_weather_ui.icon_bg = create_weather_icon_visual(s_weather_ui.page);

    lv_obj_t *caption = create_label(s_weather_ui.page, "WEATHER", FONT_TITLE,
                                     lv_color_hex(0xE6EDF5));
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, -50);

    s_weather_ui.temp_label = create_label(s_weather_ui.page, "--°", FONT_STEPS,
                                           lv_color_hex(0xF7FBFF));
    lv_obj_set_width(s_weather_ui.temp_label, 300);
    lv_obj_set_style_text_align(s_weather_ui.temp_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_weather_ui.temp_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_weather_ui.temp_label, LV_ALIGN_CENTER, 0, 6);

    s_weather_ui.condition_label = create_label(s_weather_ui.page, "--", FONT_CJK,
                                                lv_color_hex(0xFFD166));
    lv_obj_set_width(s_weather_ui.condition_label, 280);
    lv_obj_set_style_text_align(s_weather_ui.condition_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_weather_ui.condition_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_weather_ui.condition_label, LV_ALIGN_CENTER, 0, 54);

    s_weather_ui.range_label = create_label(s_weather_ui.page, "", FONT_MEDIUM,
                                            lv_color_hex(0xF7FBFF));
    lv_obj_set_width(s_weather_ui.range_label, 260);
    lv_obj_set_style_text_align(s_weather_ui.range_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_weather_ui.range_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_weather_ui.range_label, LV_ALIGN_CENTER, 0, UI_STATUS_CENTER_Y);

    s_weather_ui.status_label = create_label(s_weather_ui.page, "Open setup AP", FONT_SMALL,
                                             lv_color_hex(0x92A0AD));
    lv_obj_set_width(s_weather_ui.status_label, 320);
    lv_obj_set_style_text_align(s_weather_ui.status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_weather_ui.status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_weather_ui.status_label, LV_ALIGN_CENTER, 0, 106);

    s_weather_ui.low_value_label =
        create_metric_column(s_weather_ui.page, -UI_METRIC_COL_OFS, false, METRIC_ICON_USED,
                             "LOW", "--");
    s_weather_ui.code_value_label =
        create_metric_column(s_weather_ui.page, 0, false, METRIC_ICON_LEFT,
                             "UPDATE", "--");
    s_weather_ui.high_value_label =
        create_metric_column(s_weather_ui.page, UI_METRIC_COL_OFS, false, METRIC_ICON_LIMIT,
                             "HIGH", "--");

    s_weather_ui.updated_label = create_label(s_weather_ui.page, "", FONT_SMALL,
                                              lv_color_hex(0x63717F));
    lv_obj_set_width(s_weather_ui.updated_label, 260);
    lv_obj_set_style_text_align(s_weather_ui.updated_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_weather_ui.updated_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_weather_ui.updated_label, LV_ALIGN_CENTER, 0, UI_UPDATED_CENTER_Y);
}

static void create_pedometer_page(lv_obj_t *parent)
{
    s_pedometer_ui.page = create_page(parent);

    s_pedometer_ui.progress_arc = lv_arc_create(s_pedometer_ui.page);
    lv_obj_set_size(s_pedometer_ui.progress_arc, UI_ARC_SIZE, UI_ARC_SIZE);
    lv_obj_align(s_pedometer_ui.progress_arc, LV_ALIGN_TOP_MID, 0, UI_ARC_TOP);
    lv_arc_set_range(s_pedometer_ui.progress_arc, 0, 100);
    lv_arc_set_value(s_pedometer_ui.progress_arc, 0);
    lv_arc_set_rotation(s_pedometer_ui.progress_arc, 135);
    lv_arc_set_bg_angles(s_pedometer_ui.progress_arc, 0, 270);
    lv_obj_remove_style(s_pedometer_ui.progress_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_pedometer_ui.progress_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_pedometer_ui.progress_arc, UI_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_pedometer_ui.progress_arc, UI_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_pedometer_ui.progress_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_pedometer_ui.progress_arc, lv_color_hex(0x17212B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_pedometer_ui.progress_arc,
                               lv_color_hex(s_theme_color[THEME_STEPS]),
                               LV_PART_INDICATOR);

    s_pedometer_ui.main_icon = create_main_icon(s_pedometer_ui.page, &ui_icon_steps);

    lv_obj_t *steps_caption = create_label(s_pedometer_ui.page, "STEPS", FONT_TITLE,
                                            lv_color_hex(0xE6EDF5));
    lv_obj_align(steps_caption, LV_ALIGN_CENTER, 0, UI_CAPTION_CENTER_Y);

    s_pedometer_ui.steps_label = create_label(s_pedometer_ui.page, "0", FONT_STEPS,
                                                lv_color_hex(0xF7FBFF));
    lv_obj_set_width(s_pedometer_ui.steps_label, 300);
    lv_obj_set_style_text_align(s_pedometer_ui.steps_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_pedometer_ui.steps_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_pedometer_ui.steps_label, LV_ALIGN_CENTER, 0, UI_VALUE_CENTER_Y);

    s_pedometer_ui.goal_label = create_label(s_pedometer_ui.page, "0% OF 12K", FONT_MEDIUM,
                                             lv_color_hex(0x9DFF35));
    lv_obj_set_width(s_pedometer_ui.goal_label, 260);
    lv_obj_set_style_text_align(s_pedometer_ui.goal_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_pedometer_ui.goal_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_pedometer_ui.goal_label, LV_ALIGN_CENTER, 0, UI_GOAL_CENTER_Y);

    s_pedometer_ui.status_label = create_label(s_pedometer_ui.page, "Waiting for QMI8658",
                                                FONT_SMALL, lv_color_hex(0x92A0AD));
    lv_obj_align(s_pedometer_ui.status_label, LV_ALIGN_CENTER, 0, UI_STATUS_CENTER_Y);

    s_pedometer_ui.distance_value_label =
        create_metric_column(s_pedometer_ui.page, -UI_METRIC_COL_OFS, true, METRIC_ICON_PIN,
                             "DIST", "0.0");
    s_pedometer_ui.calories_value_label =
        create_metric_column(s_pedometer_ui.page, 0, true, METRIC_ICON_FLAME, "KCAL", "0");
    s_pedometer_ui.motion_value_label =
        create_metric_column(s_pedometer_ui.page, UI_METRIC_COL_OFS, true, METRIC_ICON_HEART,
                             "MOVE", "0");
}

static void create_codex_page(lv_obj_t *parent)
{
    s_codex_ui.page = create_page(parent);

    s_codex_ui.usage_arc = lv_arc_create(s_codex_ui.page);
    lv_obj_set_size(s_codex_ui.usage_arc, UI_ARC_SIZE, UI_ARC_SIZE);
    lv_obj_align(s_codex_ui.usage_arc, LV_ALIGN_TOP_MID, 0, UI_ARC_TOP);
    lv_arc_set_range(s_codex_ui.usage_arc, 0, 100);
    lv_arc_set_value(s_codex_ui.usage_arc, 0);
    lv_arc_set_rotation(s_codex_ui.usage_arc, 135);
    lv_arc_set_bg_angles(s_codex_ui.usage_arc, 0, 270);
    lv_obj_remove_style(s_codex_ui.usage_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_codex_ui.usage_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_codex_ui.usage_arc, UI_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_codex_ui.usage_arc, UI_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_codex_ui.usage_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_codex_ui.usage_arc, lv_color_hex(0x17212B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_codex_ui.usage_arc,
                               lv_color_hex(s_theme_color[THEME_CODEX]), LV_PART_INDICATOR);

    s_codex_ui.main_icon = create_label(s_codex_ui.page, "Codex", FONT_TITLE,
                                        lv_color_hex(0xFFD166));
    lv_obj_align(s_codex_ui.main_icon, LV_ALIGN_CENTER, 0, UI_MAIN_ICON_CENTER_Y);

    s_codex_ui.percent_label = create_label(s_codex_ui.page, "0%", FONT_STEPS,
                                              lv_color_hex(0xF7FBFF));
    lv_obj_set_width(s_codex_ui.percent_label, 300);
    lv_obj_set_style_text_align(s_codex_ui.percent_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_codex_ui.percent_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_codex_ui.percent_label, LV_ALIGN_CENTER, 0, UI_VALUE_CENTER_Y);

    s_codex_ui.label_label = create_label(s_codex_ui.page, "", FONT_SMALL,
                                           lv_color_hex(0x92A0AD));
    lv_obj_set_width(s_codex_ui.label_label, 300);
    lv_obj_set_style_text_align(s_codex_ui.label_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_codex_ui.label_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_codex_ui.label_label, LV_ALIGN_CENTER, 0, UI_STATUS_CENTER_Y);

    s_codex_ui.status_label = create_label(s_codex_ui.page, "Wi-Fi not configured", FONT_SMALL,
                                            lv_color_hex(0x92A0AD));
    lv_obj_set_width(s_codex_ui.status_label, 340);
    lv_obj_set_style_text_align(s_codex_ui.status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_codex_ui.status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_codex_ui.status_label, LV_ALIGN_CENTER, 0, UI_GOAL_CENTER_Y);

    s_codex_ui.used_value_label =
        create_metric_column(s_codex_ui.page, -UI_METRIC_COL_OFS, false, METRIC_ICON_USED,
                             "USED", "0");
    s_codex_ui.limit_value_label =
        create_metric_column(s_codex_ui.page, 0, false, METRIC_ICON_LIMIT, "LIMIT", "500K");
    s_codex_ui.left_value_label =
        create_metric_column(s_codex_ui.page, UI_METRIC_COL_OFS, false, METRIC_ICON_LEFT,
                             "LEFT", "500K");

    s_codex_ui.updated_label = create_label(s_codex_ui.page, "Updated boot", FONT_SMALL,
                                             lv_color_hex(0x63717F));
    lv_obj_set_width(s_codex_ui.updated_label, 280);
    lv_obj_set_style_text_align(s_codex_ui.updated_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_codex_ui.updated_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_codex_ui.updated_label, LV_ALIGN_CENTER, 0, UI_UPDATED_CENTER_Y);
}

static uint32_t agent_state_color(agent_state_t state)
{
    switch (state) {
    case AGENT_STATE_IDLE:
    case AGENT_STATE_DONE:
        return 0x30D158;
    case AGENT_STATE_WORKING:
        return 0xFFD60A;
    case AGENT_STATE_WAITING:
        return 0xFF9F0A;
    case AGENT_STATE_ERROR:
        return 0xFF453A;
    default:
        return 0x5A6673;
    }
}

static const char *agent_state_text(agent_state_t state)
{
    switch (state) {
    case AGENT_STATE_IDLE:
        return "IDLE";
    case AGENT_STATE_WORKING:
        return "WORKING";
    case AGENT_STATE_WAITING:
        return "WAITING";
    case AGENT_STATE_ERROR:
        return "ERROR";
    case AGENT_STATE_DONE:
        return "DONE";
    default:
        return "--";
    }
}

static const char *agent_state_short(agent_state_t state)
{
    switch (state) {
    case AGENT_STATE_IDLE:
        return "idle";
    case AGENT_STATE_WORKING:
        return "busy";
    case AGENT_STATE_WAITING:
        return "wait";
    case AGENT_STATE_ERROR:
        return "err";
    case AGENT_STATE_DONE:
        return "done";
    default:
        return "--";
    }
}

static agent_state_t agent_state_from_text(const char *text)
{
    if (text == NULL) {
        return AGENT_STATE_UNKNOWN;
    }
    if (strcmp(text, "working") == 0 || strcmp(text, "busy") == 0) {
        return AGENT_STATE_WORKING;
    }
    if (strcmp(text, "waiting") == 0) {
        return AGENT_STATE_WAITING;
    }
    if (strcmp(text, "error") == 0) {
        return AGENT_STATE_ERROR;
    }
    if (strcmp(text, "done") == 0) {
        return AGENT_STATE_DONE;
    }
    if (strcmp(text, "idle") == 0) {
        return AGENT_STATE_IDLE;
    }
    return AGENT_STATE_UNKNOWN;
}

/*
 * Lamp behaviour, matching the three-colour status light this page replaces:
 * waiting-on-you and errored blink, a finished turn breathes, idle/working
 * sit solid. Unlike the clock face this page carries no full-screen image -
 * just a thin arc and a few labels - so a real animation is affordable here
 * and the breathe can run smooth instead of stepping.
 */
typedef enum {
    AGENT_LAMP_SOLID,
    AGENT_LAMP_BLINK,
    AGENT_LAMP_BREATHE,
} agent_lamp_mode_t;

static agent_lamp_mode_t s_agent_lamp_mode = AGENT_LAMP_SOLID;
static uint8_t s_agent_lamp_anim_var;

static void agent_lamp_opa_exec_cb(void *var, int32_t value)
{
    (void)var;
    if (s_agent_ui.ring == NULL) {
        return;
    }
    lv_obj_set_style_arc_opa(s_agent_ui.ring, (lv_opa_t)value, LV_PART_INDICATOR);
    lv_obj_set_style_text_opa(s_agent_ui.state_label, (lv_opa_t)value, 0);
}

/* Restarting on every 2 s poll would reset the phase and look stuttery, so
 * the animation is only rebuilt when the mode actually changes. */
static void agent_set_lamp_mode(agent_lamp_mode_t mode)
{
    if (mode == s_agent_lamp_mode || s_agent_ui.ring == NULL) {
        return;
    }
    s_agent_lamp_mode = mode;
    lv_anim_delete(&s_agent_lamp_anim_var, agent_lamp_opa_exec_cb);

    if (mode == AGENT_LAMP_SOLID) {
        agent_lamp_opa_exec_cb(NULL, LV_OPA_COVER);
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, &s_agent_lamp_anim_var);
    lv_anim_set_exec_cb(&anim, agent_lamp_opa_exec_cb);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);

    if (mode == AGENT_LAMP_BLINK) {
        /* Also a fade, not a hard on/off: stepping the whole ring in one
         * frame exposes the 14-row strip refresh as visible banding on this
         * TE-less panel. Faster and deeper than the breathe so it still
         * reads as "needs you" rather than "resting". */
        lv_anim_set_values(&anim, LV_OPA_COVER, 60);
        lv_anim_set_duration(&anim, 520);
        lv_anim_set_reverse_duration(&anim, 520);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    } else {
        lv_anim_set_values(&anim, LV_OPA_COVER, 70);
        lv_anim_set_duration(&anim, 1300);
        lv_anim_set_reverse_duration(&anim, 1300);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    }
    lv_anim_start(&anim);
}

static void agent_refresh_lamp(void)
{
    agent_state_t state = s_agent_status.valid ? s_agent_status.state : AGENT_STATE_UNKNOWN;
    agent_lamp_mode_t mode = AGENT_LAMP_SOLID;

    /* Only animate the page you are actually looking at. */
    if (s_current_page == APP_PAGE_AGENT) {
        if (state == AGENT_STATE_WAITING || state == AGENT_STATE_ERROR) {
            mode = AGENT_LAMP_BLINK;
        } else if (state == AGENT_STATE_DONE) {
            mode = AGENT_LAMP_BREATHE;
        }
    }
    agent_set_lamp_mode(mode);
}

/*
 * The ring is a solid colour band rather than a progress arc: it is the
 * status lamp. Everything else follows the layout of the other data pages.
 */
static void create_agent_page(lv_obj_t *parent)
{
    s_agent_ui.page = create_page(parent);

    s_agent_ui.ring = lv_arc_create(s_agent_ui.page);
    lv_obj_set_size(s_agent_ui.ring, UI_ARC_SIZE, UI_ARC_SIZE);
    lv_obj_align(s_agent_ui.ring, LV_ALIGN_TOP_MID, 0, UI_ARC_TOP);
    lv_arc_set_range(s_agent_ui.ring, 0, 100);
    lv_arc_set_value(s_agent_ui.ring, 100);
    lv_arc_set_rotation(s_agent_ui.ring, 135);
    lv_arc_set_bg_angles(s_agent_ui.ring, 0, 270);
    lv_obj_remove_style(s_agent_ui.ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_agent_ui.ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_agent_ui.ring, UI_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_agent_ui.ring, UI_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_agent_ui.ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_agent_ui.ring, lv_color_hex(0x17212B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_agent_ui.ring, lv_color_hex(agent_state_color(AGENT_STATE_UNKNOWN)),
                               LV_PART_INDICATOR);

    s_agent_ui.title_label = create_label(s_agent_ui.page, "AI STATUS", FONT_TITLE,
                                          lv_color_hex(0xE6EDF5));
    lv_obj_align(s_agent_ui.title_label, LV_ALIGN_CENTER, 0, UI_MAIN_ICON_CENTER_Y);

    s_agent_ui.state_label = create_label(s_agent_ui.page, "--", FONT_VALUE,
                                          lv_color_hex(0xF7FBFF));
    lv_obj_set_width(s_agent_ui.state_label, 320);
    lv_obj_set_style_text_align(s_agent_ui.state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_agent_ui.state_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_agent_ui.state_label, LV_ALIGN_CENTER, 0, UI_VALUE_CENTER_Y);

    /* Time of the last state change, right under the state word. */
    s_agent_ui.updated_label = create_label(s_agent_ui.page, "", FONT_MEDIUM,
                                            lv_color_hex(0xC9D3DC));
    lv_obj_set_width(s_agent_ui.updated_label, 320);
    lv_obj_set_style_text_align(s_agent_ui.updated_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_agent_ui.updated_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_agent_ui.updated_label, LV_ALIGN_CENTER, 0, UI_AGENT_UPDATED_CENTER_Y);

    s_agent_ui.target_label = create_label(s_agent_ui.page, "", FONT_SMALL,
                                           lv_color_hex(0x9AA7B5));
    lv_obj_set_width(s_agent_ui.target_label, 320);
    lv_obj_set_style_text_align(s_agent_ui.target_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_agent_ui.target_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_agent_ui.target_label, LV_ALIGN_CENTER, 0, UI_GOAL_CENTER_Y);

    s_agent_ui.detail_label = create_label(s_agent_ui.page, "Bridge offline", FONT_SMALL,
                                           lv_color_hex(0x92A0AD));
    lv_obj_set_width(s_agent_ui.detail_label, 340);
    lv_obj_set_style_text_align(s_agent_ui.detail_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_agent_ui.detail_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_agent_ui.detail_label, LV_ALIGN_CENTER, 0, UI_STATUS_CENTER_Y);

    s_agent_ui.codex_value_label =
        create_metric_column(s_agent_ui.page, -UI_METRIC_COL_OFS, false, METRIC_ICON_USED,
                             "CODEX", "--");
    s_agent_ui.claude_value_label =
        create_metric_column(s_agent_ui.page, 0, false, METRIC_ICON_LEFT, "CLAUDE", "--");
    s_agent_ui.elapsed_value_label =
        create_metric_column(s_agent_ui.page, UI_METRIC_COL_OFS, false, METRIC_ICON_LIMIT,
                             "FOR", "--");

}

static uint32_t music_bar_color(int index)
{
    if (index < 5) {
        return 0x18D7F5;
    }
    if (index < 10) {
        return 0x9DFF35;
    }
    if (index < 14) {
        return 0xFFD166;
    }
    return 0xFF5A70;
}

/* Overlay page: today plus the three forecast days, one per swipe. */
static void create_forecast_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(page, page_nav_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(page, page_nav_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(page, page_nav_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(page, page_nav_event_cb, LV_EVENT_PRESS_LOST, NULL);
    s_forecast_ui.page = page;

    s_forecast_ui.range_arc = lv_arc_create(page);
    lv_obj_set_size(s_forecast_ui.range_arc, UI_ARC_SIZE, UI_ARC_SIZE);
    lv_obj_align(s_forecast_ui.range_arc, LV_ALIGN_TOP_MID, 0, UI_ARC_TOP);
    lv_arc_set_rotation(s_forecast_ui.range_arc, 135);
    lv_arc_set_bg_angles(s_forecast_ui.range_arc, 0, 270);
    lv_obj_remove_style(s_forecast_ui.range_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_forecast_ui.range_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_forecast_ui.range_arc, UI_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_forecast_ui.range_arc, UI_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_forecast_ui.range_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_forecast_ui.range_arc, lv_color_hex(0x17212B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_forecast_ui.range_arc,
                               lv_color_hex(s_theme_color[THEME_WEATHER]), LV_PART_INDICATOR);

    s_forecast_ui.day_label = create_label(page, "TODAY", FONT_MEDIUM,
                                           lv_color_hex(0x8E99A5));
    lv_obj_set_style_text_letter_space(s_forecast_ui.day_label, 3, 0);
    lv_obj_align(s_forecast_ui.day_label, LV_ALIGN_CENTER, 0, -142);

    s_forecast_ui.date_label = create_label(page, "--", FONT_SMALL, lv_color_hex(0x5F6B77));
    lv_obj_align(s_forecast_ui.date_label, LV_ALIGN_CENTER, 0, -118);

    s_forecast_ui.icon = create_weather_icon_visual(page);
    if (s_forecast_ui.icon != NULL) {
        lv_obj_align(s_forecast_ui.icon, LV_ALIGN_CENTER, 0, -62);
    }

    s_forecast_ui.range_label = create_label(page, "--", FONT_VALUE, lv_color_hex(0xF7FBFF));
    lv_obj_set_width(s_forecast_ui.range_label, 340);
    lv_obj_set_style_text_align(s_forecast_ui.range_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_forecast_ui.range_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_forecast_ui.range_label, LV_ALIGN_CENTER, 0, 20);

    s_forecast_ui.cond_label = create_label(page, "--", FONT_CJK,
                                            lv_color_hex(s_theme_color[THEME_WEATHER]));
    lv_obj_set_width(s_forecast_ui.cond_label, 300);
    lv_obj_set_style_text_align(s_forecast_ui.cond_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_forecast_ui.cond_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_forecast_ui.cond_label, LV_ALIGN_CENTER, 0, 58);

    /* One unambiguous fact per column; the daytime condition already sits
     * under the temperature range so it is not repeated here. */
    s_forecast_ui.night_value_label =
        create_metric_column(page, -UI_METRIC_COL_OFS, false, METRIC_ICON_USED, "NIGHT", "--");
    s_forecast_ui.wind_value_label =
        create_metric_column(page, 0, false, METRIC_ICON_LEFT, "WIND", "--");
    s_forecast_ui.force_value_label =
        create_metric_column(page, UI_METRIC_COL_OFS, false, METRIC_ICON_LIMIT, "FORCE", "--");

    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        lv_obj_t *dot = lv_obj_create(page);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_forecast_ui.dots[i] = dot;
    }

    set_obj_hidden(page, true);
}

static void create_music_page(lv_obj_t *parent)
{
    s_music_ui.page = create_page(parent);

    s_music_ui.vu_arc = lv_arc_create(s_music_ui.page);
    lv_obj_set_size(s_music_ui.vu_arc, UI_ARC_SIZE, UI_ARC_SIZE);
    lv_obj_align(s_music_ui.vu_arc, LV_ALIGN_TOP_MID, 0, UI_ARC_TOP);
    lv_arc_set_range(s_music_ui.vu_arc, 0, 100);
    lv_arc_set_value(s_music_ui.vu_arc, 0);
    lv_arc_set_rotation(s_music_ui.vu_arc, 135);
    lv_arc_set_bg_angles(s_music_ui.vu_arc, 0, 270);
    lv_obj_remove_style(s_music_ui.vu_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_music_ui.vu_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_music_ui.vu_arc, UI_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_music_ui.vu_arc, UI_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_music_ui.vu_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_music_ui.vu_arc, lv_color_hex(0x17212B), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_music_ui.vu_arc,
                               lv_color_hex(s_theme_color[THEME_MUSIC]), LV_PART_INDICATOR);

    /* Caption sits high so the tallest bars never crowd it. */
    lv_obj_t *caption = create_label(s_music_ui.page, "MUSIC", FONT_TITLE,
                                     lv_color_hex(0xE6EDF5));
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, -118);

    int32_t start_x = (BSP_LCD_H_RES - (MUSIC_BANDS * MUSIC_BAR_PITCH - 5)) / 2;
    for (int i = 0; i < MUSIC_BANDS; i++) {
        lv_color_t color = lv_color_hex(music_bar_color(i));

        lv_obj_t *bar = lv_obj_create(s_music_ui.page);
        lv_obj_remove_style_all(bar);
        lv_obj_set_style_bg_color(bar, color, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bar, MUSIC_BAR_WIDTH, 4);
        lv_obj_set_pos(bar, start_x + i * MUSIC_BAR_PITCH, MUSIC_BASELINE_Y - 4);
        s_music_ui.bars[i] = bar;

        lv_obj_t *peak = lv_obj_create(s_music_ui.page);
        lv_obj_remove_style_all(peak);
        lv_obj_set_style_bg_color(peak, color, 0);
        lv_obj_set_style_bg_opa(peak, LV_OPA_70, 0);
        lv_obj_set_style_radius(peak, 1, 0);
        lv_obj_clear_flag(peak, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(peak, MUSIC_BAR_WIDTH, 3);
        lv_obj_set_pos(peak, start_x + i * MUSIC_BAR_PITCH, MUSIC_BASELINE_Y - 12);
        s_music_ui.peaks[i] = peak;
    }

    lv_obj_t *baseline = lv_obj_create(s_music_ui.page);
    lv_obj_remove_style_all(baseline);
    lv_obj_set_size(baseline, 270, 2);
    lv_obj_align(baseline, LV_ALIGN_CENTER, 0, MUSIC_BASELINE_Y - 233 + 2);
    lv_obj_set_style_bg_color(baseline, lv_color_hex(0x1A2734), 0);
    lv_obj_set_style_bg_opa(baseline, LV_OPA_COVER, 0);
    lv_obj_clear_flag(baseline, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_music_ui.bass_value_label =
        create_metric_column(s_music_ui.page, -UI_METRIC_COL_OFS, false, METRIC_ICON_USED,
                             "BASS", "--");
    s_music_ui.vol_value_label =
        create_metric_column(s_music_ui.page, 0, false, METRIC_ICON_LEFT, "VOL dB", "--");
    s_music_ui.treb_value_label =
        create_metric_column(s_music_ui.page, UI_METRIC_COL_OFS, false, METRIC_ICON_LIMIT,
                             "TREB", "--");
}

static void create_page_dots(lv_obj_t *parent)
{
    s_dot_time = lv_obj_create(parent);
    lv_obj_set_size(s_dot_time, 22, 5);
    lv_obj_set_style_radius(s_dot_time, 3, 0);
    lv_obj_set_style_bg_opa(s_dot_time, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dot_time, 0, 0);
    lv_obj_clear_flag(s_dot_time, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_dot_weather = lv_obj_create(parent);
    lv_obj_set_size(s_dot_weather, 5, 5);
    lv_obj_set_style_radius(s_dot_weather, 3, 0);
    lv_obj_set_style_bg_opa(s_dot_weather, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dot_weather, 0, 0);
    lv_obj_clear_flag(s_dot_weather, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_dot_pedometer = lv_obj_create(parent);
    lv_obj_set_size(s_dot_pedometer, 5, 5);
    lv_obj_set_style_radius(s_dot_pedometer, 3, 0);
    lv_obj_set_style_bg_opa(s_dot_pedometer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dot_pedometer, 0, 0);
    lv_obj_clear_flag(s_dot_pedometer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_dot_codex = lv_obj_create(parent);
    lv_obj_set_size(s_dot_codex, 5, 5);
    lv_obj_set_style_radius(s_dot_codex, 3, 0);
    lv_obj_set_style_bg_opa(s_dot_codex, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dot_codex, 0, 0);
    lv_obj_clear_flag(s_dot_codex, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

static void settings_update_labels(void)
{
    char text[32];
    snprintf(text, sizeof(text), "BRIGHTNESS %d%%", s_cfg_brightness);
    set_label_text_if_changed(s_settings_ui.brightness_label, text);
    snprintf(text, sizeof(text), "VOLUME %d%%", s_cfg_volume);
    set_label_text_if_changed(s_settings_ui.volume_label, text);
    if (s_settings_ui.standby_checkbox != NULL) {
        snprintf(text, sizeof(text), "STANDBY %d MIN", s_cfg_standby_minutes);
        lv_checkbox_set_text(s_settings_ui.standby_checkbox, text);
    }
}

static void show_settings_panel(bool show)
{
    if (s_settings_ui.panel == NULL) {
        return;
    }
    s_settings_visible = show;
    set_obj_hidden(s_settings_ui.panel, !show);
    if (show) {
        settings_update_labels();
        lv_obj_move_foreground(s_settings_ui.panel);
    }
}

static void settings_brightness_event_cb(lv_event_t *event)
{
    s_last_activity_ms = now_ms();
    int32_t value = lv_slider_get_value(s_settings_ui.brightness_slider);
    s_cfg_brightness = (uint8_t)value;
    (void)bsp_display_brightness_set((int)value);
    settings_update_labels();
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        save_ui_settings();
    }
}

static void settings_volume_event_cb(lv_event_t *event)
{
    s_last_activity_ms = now_ms();
    s_cfg_volume = (uint8_t)lv_slider_get_value(s_settings_ui.volume_slider);
    settings_update_labels();
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        save_ui_settings();
    }
}

static void settings_standby_switch_event_cb(lv_event_t *event)
{
    (void)event;
    s_last_activity_ms = now_ms();
    s_cfg_standby_enabled = lv_obj_has_state(s_settings_ui.standby_checkbox, LV_STATE_CHECKED);
    save_ui_settings();
}

static void settings_standby_slider_event_cb(lv_event_t *event)
{
    s_last_activity_ms = now_ms();
    s_cfg_standby_minutes = (uint8_t)lv_slider_get_value(s_settings_ui.standby_slider);
    settings_update_labels();
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        save_ui_settings();
    }
}

/* Swipe up anywhere on the panel background to close it. */
static void settings_panel_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL) {
        indev = lv_indev_active();
    }
    if (indev == NULL) {
        return;
    }

    s_last_activity_ms = now_ms();
    if (code == LV_EVENT_PRESSED) {
        wake_screen();
        lv_indev_get_point(indev, &s_settings_press_point);
        s_settings_press_active = true;
        return;
    }
    if (code == LV_EVENT_PRESS_LOST) {
        s_settings_press_active = false;
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_settings_press_active) {
        return;
    }
    s_settings_press_active = false;

    lv_point_t release_point;
    lv_indev_get_point(indev, &release_point);
    int dy = (int)release_point.y - (int)s_settings_press_point.y;
    if (-dy >= PAGE_SWIPE_MIN_PX) {
        show_settings_panel(false);
    }
}

static lv_obj_t *create_settings_slider(lv_obj_t *parent, int32_t y, int32_t min_value,
                                        int32_t max_value, int32_t value, lv_event_cb_t cb)
{
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, 240, 14);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, y);
    lv_slider_set_range(slider, min_value, max_value);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A2E33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x18D7F5), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xF2F2F7), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_RELEASED, NULL);
    return slider;
}

static void create_settings_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(panel, settings_panel_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(panel, settings_panel_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(panel, settings_panel_event_cb, LV_EVENT_PRESS_LOST, NULL);
    s_settings_ui.panel = panel;

    lv_obj_t *title = create_label(panel, "SETTINGS", FONT_TITLE, lv_color_hex(0xF7FBFF));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 62);

    s_settings_ui.brightness_label = create_label(panel, "BRIGHTNESS", FONT_SMALL,
                                                  lv_color_hex(0x9AA7B5));
    lv_obj_align(s_settings_ui.brightness_label, LV_ALIGN_TOP_MID, 0, 116);
    s_settings_ui.brightness_slider =
        create_settings_slider(panel, 142, SETTINGS_BRIGHTNESS_MIN, 100, s_cfg_brightness,
                               settings_brightness_event_cb);

    s_settings_ui.volume_label = create_label(panel, "VOLUME", FONT_SMALL,
                                              lv_color_hex(0x9AA7B5));
    lv_obj_align(s_settings_ui.volume_label, LV_ALIGN_TOP_MID, 0, 190);
    s_settings_ui.volume_slider =
        create_settings_slider(panel, 216, 0, 100, s_cfg_volume, settings_volume_event_cb);

    s_settings_ui.standby_checkbox = lv_checkbox_create(panel);
    lv_checkbox_set_text(s_settings_ui.standby_checkbox, "STANDBY 3 MIN");
    lv_obj_align(s_settings_ui.standby_checkbox, LV_ALIGN_TOP_MID, 0, 262);
    lv_obj_set_style_text_color(s_settings_ui.standby_checkbox, lv_color_hex(0xF7FBFF), 0);
    lv_obj_set_style_text_font(s_settings_ui.standby_checkbox, FONT_SMALL, 0);
    lv_obj_set_style_border_color(s_settings_ui.standby_checkbox, lv_color_hex(0x657181),
                                  LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_settings_ui.standby_checkbox, lv_color_hex(0x101820),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_settings_ui.standby_checkbox, lv_color_hex(0x18D7F5),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (s_cfg_standby_enabled) {
        lv_obj_add_state(s_settings_ui.standby_checkbox, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_settings_ui.standby_checkbox, settings_standby_switch_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_settings_ui.standby_slider =
        create_settings_slider(panel, 306, SETTINGS_STANDBY_MIN_MINUTES,
                               SETTINGS_STANDBY_MAX_MINUTES, s_cfg_standby_minutes,
                               settings_standby_slider_event_cb);

    lv_obj_t *hint = create_label(panel, "Swipe up to close", FONT_SMALL,
                                  lv_color_hex(0x63717F));
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 362);

    settings_update_labels();
    set_obj_hidden(panel, true);
}

static void create_app_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    create_music_page(scr);
    create_agent_page(scr);
    create_forecast_page(scr);
    create_codex_page(scr);
    create_pedometer_page(scr);
    create_weather_page(scr);
    create_time_page(scr);
    create_status_bar(scr);
    create_page_dots(scr);
    create_settings_panel(scr);
    set_active_page_locked(APP_PAGE_TIME);
}

static void request_full_screen_refresh(void)
{
    if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) == ESP_OK) {
        lv_obj_invalidate(lv_screen_active());
        bsp_display_unlock();
    }
}

static bool area_inside_square(const lv_area_t *area)
{
    return area->x1 >= 0 && area->y1 >= 0 &&
           area->x2 < BSP_LCD_H_RES && area->y2 < BSP_LCD_V_RES;
}

static bool point_inside_round_visible_area(int32_t x, int32_t y)
{
    int32_t cx = BSP_LCD_H_RES / 2;
    int32_t cy = BSP_LCD_V_RES / 2;
    int32_t dx = x - cx;
    int32_t dy = y - cy;
    int32_t radius = UI_ROUND_VISIBLE_RADIUS;
    return (dx * dx + dy * dy) <= (radius * radius);
}

static bool area_inside_round_visible_area(const lv_area_t *area)
{
    return point_inside_round_visible_area(area->x1, area->y1) &&
           point_inside_round_visible_area(area->x2, area->y1) &&
           point_inside_round_visible_area(area->x1, area->y2) &&
           point_inside_round_visible_area(area->x2, area->y2);
}

static bool arc_inside_round_visible_area(const lv_area_t *area)
{
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    int32_t radius = (w > h ? w : h) / 2;
    int32_t cx = area->x1 + w / 2;
    int32_t cy = area->y1 + h / 2;
    int32_t dx = cx - BSP_LCD_H_RES / 2;
    int32_t dy = cy - BSP_LCD_V_RES / 2;
    int32_t remaining = UI_ROUND_VISIBLE_RADIUS - radius;

    if (remaining < 0) {
        return false;
    }
    return (dx * dx + dy * dy) <= (remaining * remaining);
}

static void get_estimated_draw_area(lv_obj_t *obj, lv_area_t *area)
{
    lv_obj_get_coords(obj, area);

    int32_t scale_x = lv_obj_get_style_transform_scale_x(obj, LV_PART_MAIN);
    int32_t scale_y = lv_obj_get_style_transform_scale_y(obj, LV_PART_MAIN);
    int32_t extra_w = lv_obj_get_style_transform_width(obj, LV_PART_MAIN);
    int32_t extra_h = lv_obj_get_style_transform_height(obj, LV_PART_MAIN);

    if (scale_x <= 0) {
        scale_x = UI_LABEL_SCALE_NONE;
    }
    if (scale_y <= 0) {
        scale_y = UI_LABEL_SCALE_NONE;
    }

    if (scale_x == UI_LABEL_SCALE_NONE && scale_y == UI_LABEL_SCALE_NONE &&
        extra_w == 0 && extra_h == 0) {
        return;
    }

    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    int32_t cx = area->x1 + w / 2;
    int32_t cy = area->y1 + h / 2;
    int32_t draw_w = (w * scale_x + UI_LABEL_SCALE_NONE / 2) / UI_LABEL_SCALE_NONE +
                     (extra_w > 0 ? extra_w * 2 : 0);
    int32_t draw_h = (h * scale_y + UI_LABEL_SCALE_NONE / 2) / UI_LABEL_SCALE_NONE +
                     (extra_h > 0 ? extra_h * 2 : 0);

    area->x1 = cx - draw_w / 2;
    area->x2 = area->x1 + draw_w - 1;
    area->y1 = cy - draw_h / 2;
    area->y2 = area->y1 + draw_h - 1;
}

static void print_area_report(const char *kind, const char *name, lv_obj_t *obj,
                              bool estimate_transform)
{
    if (obj == NULL) {
        printf("%s name=%s missing\n", kind, name);
        return;
    }

    lv_area_t area;
    if (estimate_transform) {
        get_estimated_draw_area(obj, &area);
    } else {
        lv_obj_get_coords(obj, &area);
    }

    bool square_ok = area_inside_square(&area);
    bool round_ok = strcmp(kind, "ARC") == 0 ? arc_inside_round_visible_area(&area) :
                    area_inside_round_visible_area(&area);

    printf("%s name=%s x1=%ld y1=%ld x2=%ld y2=%ld w=%ld h=%ld square=%s round=%s\n",
           kind, name, (long)area.x1, (long)area.y1, (long)area.x2, (long)area.y2,
           (long)lv_area_get_width(&area), (long)lv_area_get_height(&area),
           square_ok ? "OK" : "WARN", round_ok ? "OK" : "WARN");
}

static void print_label_text_report(const char *name, lv_obj_t *label)
{
    const char *text = label != NULL ? lv_label_get_text(label) : NULL;
    printf("TEXT name=%s value=\"%s\"\n", name, text != NULL ? text : "<missing>");
}

static void dump_layout_report(void)
{
    if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) != ESP_OK) {
        printf("__LVGL_LAYOUT_ERROR__ lock_failed\n");
        return;
    }

    lv_obj_update_layout(lv_screen_active());

    flockfile(stdout);
    printf("__LVGL_LAYOUT_BEGIN__ screen=%dx%d round_cx=%d round_cy=%d round_r=%d page=%s\n",
           BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_H_RES / 2, BSP_LCD_V_RES / 2,
           UI_ROUND_VISIBLE_RADIUS, page_name(s_current_page));

    print_area_report("OBJ", "status_clock", s_status_ui.clock_icon, false);
    print_area_report("OBJ", "status_time", s_status_ui.time_label, false);
    print_area_report("OBJ", "status_title", s_status_ui.title_label, false);
    print_area_report("OBJ", "status_battery", s_status_ui.battery_icon, false);

    print_area_report("OBJ", "time_digital", s_time_ui.digital_label, true);
    print_area_report("OBJ", "time_status", s_time_ui.status_label, false);
    print_area_report("ARC", "corner0_arc", s_time_ui.corner_arc[0], false);
    print_area_report("ARC", "corner1_arc", s_time_ui.corner_arc[1], false);
    print_area_report("OBJ", "corner2_value", s_time_ui.corner_value[2], false);
    print_area_report("OBJ", "corner3_value", s_time_ui.corner_value[3], false);

    print_area_report("ARC", "weather_arc", s_weather_ui.temp_arc, false);
    print_area_report("OBJ", "weather_icon_bg", s_weather_ui.icon_bg, false);
    print_area_report("OBJ", "weather_city", s_weather_ui.city_label, false);
    print_area_report("OBJ", "weather_temp", s_weather_ui.temp_label, true);
    print_area_report("OBJ", "weather_condition", s_weather_ui.condition_label, false);
    print_area_report("OBJ", "weather_range", s_weather_ui.range_label, false);
    print_area_report("OBJ", "weather_status", s_weather_ui.status_label, false);

    print_area_report("ARC", "pedometer_arc", s_pedometer_ui.progress_arc, false);
    print_area_report("OBJ", "pedometer_icon", s_pedometer_ui.main_icon, false);
    print_area_report("OBJ", "steps_value", s_pedometer_ui.steps_label, true);
    print_area_report("OBJ", "steps_goal", s_pedometer_ui.goal_label, true);
    print_area_report("OBJ", "steps_status", s_pedometer_ui.status_label, false);
    print_area_report("OBJ", "dist_value", s_pedometer_ui.distance_value_label, true);
    print_area_report("OBJ", "kcal_value", s_pedometer_ui.calories_value_label, true);
    print_area_report("OBJ", "motion_value", s_pedometer_ui.motion_value_label, true);

    print_area_report("ARC", "codex_arc", s_codex_ui.usage_arc, false);
    print_area_report("OBJ", "codex_icon", s_codex_ui.main_icon, false);
    print_area_report("OBJ", "codex_percent", s_codex_ui.percent_label, true);
    print_area_report("OBJ", "codex_label", s_codex_ui.label_label, false);
    print_area_report("OBJ", "codex_status", s_codex_ui.status_label, false);
    print_area_report("OBJ", "used_value", s_codex_ui.used_value_label, true);
    print_area_report("OBJ", "limit_value", s_codex_ui.limit_value_label, true);
    print_area_report("OBJ", "left_value", s_codex_ui.left_value_label, true);
    print_area_report("OBJ", "page_dot_time", s_dot_time, false);
    print_area_report("OBJ", "page_dot_weather", s_dot_weather, false);
    print_area_report("OBJ", "page_dot_pedometer", s_dot_pedometer, false);
    print_area_report("OBJ", "page_dot_codex", s_dot_codex, false);

    print_label_text_report("time_digital", s_time_ui.digital_label);
    print_label_text_report("time_status", s_time_ui.status_label);
    print_label_text_report("corner0", s_time_ui.corner_value[0]);
    print_label_text_report("corner1", s_time_ui.corner_value[1]);
    print_label_text_report("corner2", s_time_ui.corner_value[2]);
    print_label_text_report("corner3", s_time_ui.corner_value[3]);
    print_label_text_report("weather_city", s_weather_ui.city_label);
    print_label_text_report("weather_temp", s_weather_ui.temp_label);
    print_label_text_report("weather_condition", s_weather_ui.condition_label);
    print_label_text_report("steps_value", s_pedometer_ui.steps_label);
    print_label_text_report("codex_percent", s_codex_ui.percent_label);
    print_label_text_report("codex_status", s_codex_ui.status_label);
    print_label_text_report("status_time", s_status_ui.time_label);

    printf("MEM internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    printf("__LVGL_LAYOUT_END__\n");
    fflush(stdout);
    funlockfile(stdout);

    bsp_display_unlock();
}

static void process_console_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }

    if (strcmp(line, "layout") == 0) {
        dump_layout_report();
    } else if (strcmp(line, "shot") == 0 || strcmp(line, "screenshot") == 0) {
        printf("__LVGL_SCREENSHOT_ERROR__ snapshot_disabled_use_layout\n");
    } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        printf("Commands: layout, help\n");
    } else if (len > 0) {
        printf("Unknown command: %s\n", line);
    }
}

static void screenshot_console_task(void *arg)
{
    (void)arg;
    char line[32];
    size_t index = 0;

    printf("Debug console ready. Type 'layout' to dump LVGL object bounds.\n");
    fflush(stdout);

    vTaskDelay(pdMS_TO_TICKS(DEBUG_AUTO_DELAY_MS));
    dump_layout_report();

    /* Second dump proves the clock label keeps advancing after boot. */
    vTaskDelay(pdMS_TO_TICKS(3 * DEBUG_AUTO_DELAY_MS));
    dump_layout_report();

    while (true) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            line[index] = '\0';
            process_console_line(line);
            index = 0;
        } else if (index < sizeof(line) - 1) {
            line[index++] = (char)ch;
        }
    }
}

static void update_time_page_locked(void)
{
    if (s_time_ui.page == NULL || s_time_ui.digital_label == NULL) {
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm local_time;
    localtime_r(&tv.tv_sec, &local_time);

    /* The second hand sweeps: update it on every 100 ms tick with sub-second
     * precision. Everything else only changes once per second. */
    float second_f = (float)local_time.tm_sec + (float)tv.tv_usec / 1000000.0f;
    set_hand_angle(s_time_ui.second_hand, s_second_hand_points, second_f * 6.0f,
                   WATCH_SECOND_HAND_LEN, WATCH_SECOND_TAIL_LEN, 3);

    static time_t last_rendered_second;
    if (tv.tv_sec == last_rendered_second) {
        return;
    }
    last_rendered_second = tv.tv_sec;

    float minute_angle = ((float)local_time.tm_min + (float)local_time.tm_sec / 60.0f) * 6.0f;
    float hour_angle = ((float)(local_time.tm_hour % 12) +
                        (float)local_time.tm_min / 60.0f) * 30.0f;
    set_hand_angle(s_time_ui.hour_hand, s_hour_hand_points, hour_angle,
                   WATCH_HOUR_HAND_LEN, WATCH_HAND_TAIL_LEN, 9);
    set_hand_angle(s_time_ui.minute_hand, s_minute_hand_points, minute_angle,
                   WATCH_MINUTE_HAND_LEN, WATCH_HAND_TAIL_LEN, 6);

    char text[32];
    snprintf(text, sizeof(text), "%02d:%02d:%02d", local_time.tm_hour, local_time.tm_min,
             local_time.tm_sec);
    set_label_text_if_changed(s_time_ui.digital_label, text);

    for (int corner = 0; corner < 4; corner++) {
        corner_render_widget_locked(corner, &local_time);
    }

    EventBits_t bits = s_wifi_event_group != NULL ? xEventGroupGetBits(s_wifi_event_group) : 0;
    bool configured = wifi_configured();
    set_obj_hidden(s_time_ui.status_label, configured);
    set_obj_hidden(s_time_ui.wifi_icon, !configured);
    set_obj_hidden(s_time_ui.ntp_icon, !configured);

    if (configured) {
        uint32_t wifi_color;
        if ((bits & WIFI_CONNECTED_BIT) != 0) {
            wifi_color = WATCH_COLOR_BATTERY;
        } else if ((bits & WIFI_FAIL_BIT) != 0) {
            wifi_color = WATCH_COLOR_MONTH;
        } else {
            wifi_color = WATCH_COLOR_CAPTION;
        }
        lv_obj_set_style_text_color(s_time_ui.wifi_icon, lv_color_hex(wifi_color), 0);
        lv_obj_set_style_text_color(s_time_ui.ntp_icon,
                                    lv_color_hex(s_time_synced ? WATCH_COLOR_BATTERY
                                                               : WATCH_COLOR_CAPTION), 0);
    }
}

static void render_time_page(void)
{
    if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }

    if (s_current_page == APP_PAGE_TIME) {
        update_time_page_locked();
    }

    update_status_bar_locked();
    bsp_display_unlock();
}

static void init_battery_monitor(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_axp2101_dev) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not available; battery gauge disabled");
        s_axp2101_dev = NULL;
    }
}

static void refresh_battery_percent(void)
{
    if (s_axp2101_dev == NULL) {
        return;
    }

    uint8_t reg = AXP2101_REG_BATT_PERCENT;
    uint8_t value = 0;
    esp_err_t ret = i2c_master_transmit_receive(s_axp2101_dev, &reg, 1, &value, 1, 100);
    if (ret == ESP_OK && value <= 100) {
        s_battery_percent = value;
    }
}

static void clock_task(void *arg)
{
    (void)arg;
    uint32_t last_battery_ms = 0;
    bool battery_read_once = false;

    while (true) {
        uint32_t current_ms = now_ms();
        if (!battery_read_once || (current_ms - last_battery_ms) >= BATTERY_POLL_MS) {
            battery_read_once = true;
            last_battery_ms = current_ms;
            refresh_battery_percent();
        }

        if (s_ntp_resync_requested) {
            s_ntp_resync_requested = false;
            s_time_synced = false;
            s_ntp_sync_in_progress = false;
            s_last_ntp_attempt_ms = 0;
            (void)request_ntp_sync();
        }

        if (s_cfg_standby_enabled && !s_screen_off &&
            s_current_page != APP_PAGE_MUSIC &&
            (current_ms - s_last_activity_ms) >=
                (uint32_t)s_cfg_standby_minutes * 60000U) {
            s_screen_off = true;
            (void)bsp_display_brightness_set(0);
        }

        render_time_page();
        vTaskDelay(pdMS_TO_TICKS(CLOCK_TASK_DELAY_MS));
    }
}

static void render_pedometer(uint32_t steps, int motion_mg, const char *status_text)
{
    uint32_t goal_steps = s_pedometer_goal_steps >= PEDOMETER_MIN_GOAL_STEPS
                              ? s_pedometer_goal_steps
                              : PEDOMETER_DEFAULT_GOAL_STEPS;
    uint32_t progress =
        clamp_u32((uint32_t)(((uint64_t)steps * 100U) / goal_steps), 100U);
    uint32_t distance_tenths_km = (steps * 7U) / 1000U;
    uint32_t calories = (steps * 4U) / 100U;

    if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }

    if (lv_arc_get_value(s_pedometer_ui.progress_arc) != (int32_t)progress) {
        lv_arc_set_value(s_pedometer_ui.progress_arc, (int32_t)progress);
    }
    char steps_text[16];
    format_step_count(steps, steps_text, sizeof(steps_text));

    char goal_text[24];
    char distance_text[16];
    char calories_text[16];
    char motion_text[16];
    if (goal_steps >= 1000U) {
        uint32_t goal_tenths_k = (goal_steps + 50U) / 100U;
        if ((goal_tenths_k % 10U) == 0U) {
            snprintf(goal_text, sizeof(goal_text), "%lu%% OF %luK",
                     (unsigned long)progress, (unsigned long)(goal_tenths_k / 10U));
        } else {
            snprintf(goal_text, sizeof(goal_text), "%lu%% OF %lu.%luK",
                     (unsigned long)progress, (unsigned long)(goal_tenths_k / 10U),
                     (unsigned long)(goal_tenths_k % 10U));
        }
    } else {
        snprintf(goal_text, sizeof(goal_text), "%lu%% OF %lu",
                 (unsigned long)progress, (unsigned long)goal_steps);
    }
    snprintf(distance_text, sizeof(distance_text), "%lu.%lu",
             (unsigned long)(distance_tenths_km / 10U),
             (unsigned long)(distance_tenths_km % 10U));
    snprintf(calories_text, sizeof(calories_text), "%lu", (unsigned long)calories);
    snprintf(motion_text, sizeof(motion_text), "%d", motion_mg);

    set_label_text_if_changed(s_pedometer_ui.steps_label, steps_text);
    set_label_text_if_changed(s_pedometer_ui.goal_label, goal_text);
    set_label_text_if_changed(s_pedometer_ui.status_label, status_text);
    set_label_text_if_changed(s_pedometer_ui.distance_value_label, distance_text);
    set_label_text_if_changed(s_pedometer_ui.calories_value_label, calories_text);
    set_label_text_if_changed(s_pedometer_ui.motion_value_label, motion_text);
    update_status_bar_locked();

    bsp_display_unlock();
}

static uint32_t utf8_next_codepoint(const char *text, size_t *index)
{
    const unsigned char *p = (const unsigned char *)text + *index;
    uint32_t cp;
    int len;

    if (p[0] == 0) {
        return 0;
    }
    if (p[0] < 0x80) {
        cp = p[0];
        len = 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        len = 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        len = 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07;
        len = 4;
    } else {
        (*index)++;
        return 0xFFFD;
    }

    for (int k = 1; k < len; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            *index += (size_t)k;
            return 0xFFFD;
        }
        cp = (cp << 6) | (uint32_t)(p[k] & 0x3F);
    }
    *index += (size_t)len;
    return cp;
}

/* The bundled CJK font is a subset (it lacks 云/阴/雷/雾 among others), so
 * verify every glyph before trusting a server-provided Chinese string. */
static bool text_glyphs_available(const lv_font_t *font, const char *text)
{
    size_t i = 0;
    for (;;) {
        uint32_t cp = utf8_next_codepoint(text, &i);
        if (cp == 0) {
            return true;
        }
        if (cp < 0x80) {
            continue;
        }
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(font, &dsc, cp, ' ') ||
            (dsc.box_w == 0 && dsc.adv_w == 0)) {
            return false;
        }
    }
}

static const char *weather_condition_en(int code)
{
    if (code == 0) {
        return "Sunny";
    }
    if (code == 2) {
        return "Cloudy";
    }
    if (code == 3) {
        return "Overcast";
    }
    if (code == 45 || code == 48) {
        return "Foggy";
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        return "Rain";
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return "Snow";
    }
    if (code >= 95) {
        return "Storm";
    }
    return "Cloudy";
}

static uint32_t weather_accent_color(int code)
{
    if (code == 0) {
        return 0xFFD166;
    }
    if (code == 45 || code == 48) {
        return 0xA1AAB5;
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        return 0x18D7F5;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return 0xD9F4FF;
    }
    if (code >= 95) {
        return 0xFFD60A;
    }
    return 0x8EA4B8;
}

static void render_weather(const weather_data_t *weather, const char *status_text)
{
    const weather_data_t *data = weather != NULL ? weather : &s_last_weather;
    uint32_t accent = weather_accent_color(data->weather_code);

    char temp_text[16];
    char range_text[32];
    char low_text[16];
    char high_text[16];
    char update_short_text[16];
    char display_city[WEATHER_CITY_STORAGE_SIZE];

    build_weather_display_city(data->city, display_city, sizeof(display_city));
    if (data->valid) {
        snprintf(temp_text, sizeof(temp_text), "%d°", data->current_temp_c);
        range_text[0] = '\0';
        snprintf(low_text, sizeof(low_text), "%d°", data->min_temp_c);
        snprintf(high_text, sizeof(high_text), "%d°", data->max_temp_c);
        const char *space = strchr(data->updated, ' ');
        size_t update_len = space != NULL ? (size_t)(space - data->updated) : strlen(data->updated);
        if (update_len >= sizeof(update_short_text)) {
            update_len = sizeof(update_short_text) - 1;
        }
        memcpy(update_short_text, data->updated, update_len);
        update_short_text[update_len] = '\0';
    } else {
        snprintf(temp_text, sizeof(temp_text), "--°");
        range_text[0] = '\0';
        snprintf(low_text, sizeof(low_text), "--");
        snprintf(high_text, sizeof(high_text), "--");
        snprintf(update_short_text, sizeof(update_short_text), "--");
    }

    if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }

    if (lv_arc_get_value(s_weather_ui.temp_arc) !=
        clamp_int(data->current_temp_c, -30, 45)) {
        lv_arc_set_value(s_weather_ui.temp_arc, clamp_int(data->current_temp_c, -30, 45));
    }
    lv_obj_set_style_arc_color(s_weather_ui.temp_arc, lv_color_hex(accent),
                               LV_PART_INDICATOR);

    static int last_icon_code = -999;
    int icon_code = data->valid ? data->weather_code : -1;
    if (icon_code != last_icon_code) {
        last_icon_code = icon_code;
        draw_weather_icon(s_weather_ui.icon_bg, icon_code);
    }
    const char *condition_text = "--";
    if (data->valid) {
        condition_text = text_glyphs_available(FONT_CJK, data->condition)
                             ? data->condition
                             : weather_condition_en(data->weather_code);
    }

    set_label_text_if_changed(s_weather_ui.city_label, display_city);
    set_label_text_if_changed(s_weather_ui.temp_label, temp_text);
    set_label_text_if_changed(s_weather_ui.condition_label, condition_text);
    set_label_text_if_changed(s_weather_ui.range_label, range_text);
    set_label_text_if_changed(s_weather_ui.status_label, status_text);
    set_label_text_if_changed(s_weather_ui.low_value_label, low_text);
    set_label_text_if_changed(s_weather_ui.high_value_label, high_text);
    set_label_text_if_changed(s_weather_ui.code_value_label, update_short_text);
    set_label_text_if_changed(s_weather_ui.updated_label, "");
    update_status_bar_locked();

    bsp_display_unlock();
}

static void render_weather_status(const char *status_text)
{
    render_weather(&s_last_weather, status_text);
}

/* Paint one forecast day. The ring draws that day's low..high as a segment
 * positioned inside the span of all four days, so relative warmth and swing
 * read at a glance. */
static void render_forecast_locked(void)
{
    if (s_forecast_ui.page == NULL) {
        return;
    }

    int count = s_last_weather.day_count;
    if (count <= 0) {
        set_label_text_if_changed(s_forecast_ui.day_label, "NO DATA");
        set_label_text_if_changed(s_forecast_ui.date_label, "");
        set_label_text_if_changed(s_forecast_ui.range_label, "--");
        set_label_text_if_changed(s_forecast_ui.cond_label, "");
        return;
    }
    if (s_forecast_day >= count) {
        s_forecast_day = 0;
    }

    int span_low = 1000;
    int span_high = -1000;
    for (int i = 0; i < count; i++) {
        if (s_last_weather.days[i].night_temp_c < span_low) {
            span_low = s_last_weather.days[i].night_temp_c;
        }
        if (s_last_weather.days[i].day_temp_c > span_high) {
            span_high = s_last_weather.days[i].day_temp_c;
        }
    }
    if (span_high <= span_low) {
        span_high = span_low + 1;
    }

    const weather_day_t *day = &s_last_weather.days[s_forecast_day];
    lv_arc_set_range(s_forecast_ui.range_arc, span_low, span_high);
    /* Both ends move, so drive the indicator angles directly instead of a
     * value: an arc value always starts from the range minimum. */
    int32_t start_angle =
        (int32_t)(270L * (day->night_temp_c - span_low) / (span_high - span_low));
    int32_t end_angle =
        (int32_t)(270L * (day->day_temp_c - span_low) / (span_high - span_low));
    if (end_angle <= start_angle) {
        end_angle = start_angle + 6;
    }
    lv_arc_set_angles(s_forecast_ui.range_arc, start_angle, end_angle);

    static const char *const day_names[WEATHER_FORECAST_DAYS] = {"TODAY", "DAY 2", "DAY 3",
                                                                 "DAY 4"};
    set_label_text_if_changed(s_forecast_ui.day_label, day_names[s_forecast_day]);
    set_label_text_if_changed(s_forecast_ui.date_label, day->date);

    char text[32];
    snprintf(text, sizeof(text), "%d°~%d°", day->night_temp_c, day->day_temp_c);
    set_label_text_if_changed(s_forecast_ui.range_label, text);

    /* The centre line uses the CJK font, so Chinese is fine there. */
    set_label_text_if_changed(s_forecast_ui.cond_label,
                              text_glyphs_available(FONT_CJK, day->day_cond)
                                  ? day->day_cond
                                  : weather_condition_en(day->day_code));

    /* Metric columns render in Montserrat, which carries no CJK at all, so
     * their values must stay ASCII regardless of what the API returned. */
    set_label_text_if_changed(s_forecast_ui.night_value_label,
                              weather_condition_en(day->night_code));
    set_label_text_if_changed(s_forecast_ui.wind_value_label,
                              day->wind_dir[0] != '\0' ? day->wind_dir : "--");
    set_label_text_if_changed(s_forecast_ui.force_value_label,
                              day->wind_power[0] != '\0' ? day->wind_power : "--");

    draw_weather_icon(s_forecast_ui.icon, day->day_code);

    for (int i = 0; i < WEATHER_FORECAST_DAYS; i++) {
        bool active = i == s_forecast_day;
        set_obj_hidden(s_forecast_ui.dots[i], i >= count);
        lv_obj_set_size(s_forecast_ui.dots[i], active ? 22 : 5, 5);
        lv_obj_set_style_bg_color(s_forecast_ui.dots[i],
                                  lv_color_hex(active ? s_theme_color[THEME_WEATHER]
                                                      : 0x32404E),
                                  0);
        lv_obj_align(s_forecast_ui.dots[i], LV_ALIGN_CENTER,
                     -33 + i * (active ? 22 : 22), 191);
    }
}

static void show_forecast_page(bool show)
{
    if (s_forecast_ui.page == NULL) {
        return;
    }
    s_forecast_open = show;
    if (show) {
        s_forecast_day = 0;
        render_forecast_locked();
        lv_obj_move_foreground(s_forecast_ui.page);
    }
    set_obj_hidden(s_forecast_ui.page, !show);
    lv_obj_invalidate(lv_screen_active());
}

static void format_compact_u64(uint64_t value, char *buffer, size_t buffer_size)
{
    if (value >= 1000000ULL) {
        uint64_t whole = value / 1000000ULL;
        uint64_t decimal = (value % 1000000ULL) / 100000ULL;
        if (whole < 100ULL && decimal > 0ULL) {
            snprintf(buffer, buffer_size, "%llu.%lluM", (unsigned long long)whole,
                     (unsigned long long)decimal);
        } else {
            snprintf(buffer, buffer_size, "%lluM", (unsigned long long)whole);
        }
    } else if (value >= 1000ULL) {
        uint64_t whole = value / 1000ULL;
        uint64_t decimal = (value % 1000ULL) / 100ULL;
        if (whole < 100ULL && decimal > 0ULL) {
            snprintf(buffer, buffer_size, "%llu.%lluK", (unsigned long long)whole,
                     (unsigned long long)decimal);
        } else {
            snprintf(buffer, buffer_size, "%lluK", (unsigned long long)whole);
        }
    } else {
        snprintf(buffer, buffer_size, "%llu", (unsigned long long)value);
    }
}

static void format_codex_reset_text(const char *updated, char *buffer, size_t buffer_size)
{
    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    if (updated == NULL || updated[0] == '\0') {
        return;
    }

    const char *reset = strstr(updated, "reset ");
    if (reset == NULL) {
        reset = strstr(updated, "Reset ");
    }
    if (reset == NULL) {
        return;
    }

    reset += strlen("reset ");
    while (*reset == ' ' || *reset == ',') {
        reset++;
    }
    snprintf(buffer, buffer_size, "Reset %s", reset);
}

static void render_codex_usage(const codex_usage_t *usage, const char *status_text)
{
    uint64_t used = usage->used_tokens;
    uint64_t limit = usage->limit_tokens;
    uint64_t left = limit > used ? limit - used : 0;
    uint32_t percent = limit > 0 ? (uint32_t)((used * 100ULL) / limit) : 0;
    percent = clamp_u32(percent, 100U);

    char used_text[16];
    char limit_text[16];
    char left_text[16];
    format_compact_u64(used, used_text, sizeof(used_text));
    format_compact_u64(limit, limit_text, sizeof(limit_text));
    format_compact_u64(left, left_text, sizeof(left_text));

    if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }

    if (lv_arc_get_value(s_codex_ui.usage_arc) != (int32_t)percent) {
        lv_arc_set_value(s_codex_ui.usage_arc, (int32_t)percent);
    }

    char percent_text[16];
    char reset_text[48];
    snprintf(percent_text, sizeof(percent_text), "%lu%%", (unsigned long)percent);
    format_codex_reset_text(usage->updated, reset_text, sizeof(reset_text));

    bool online = strcmp(status_text, "Online") == 0;

    set_label_text_if_changed(s_codex_ui.percent_label, percent_text);
    set_label_text_if_changed(s_codex_ui.label_label, online ? reset_text : "");
    set_label_text_if_changed(s_codex_ui.status_label, status_text);
    lv_obj_set_style_text_color(s_codex_ui.status_label,
                                lv_color_hex(online ? 0x9DFF35 : 0x92A0AD), 0);
    set_label_text_if_changed(s_codex_ui.used_value_label, used_text);
    set_label_text_if_changed(s_codex_ui.limit_value_label, limit_text);
    set_label_text_if_changed(s_codex_ui.left_value_label, left_text);
    set_label_text_if_changed(s_codex_ui.updated_label, "");
    update_status_bar_locked();

    bsp_display_unlock();
}

static void render_codex_status(const char *status_text)
{
    render_codex_usage(&s_last_codex_usage, status_text);
}

static void format_elapsed_short(uint32_t seconds, char *buffer, size_t buffer_size)
{
    if (seconds >= 3600U) {
        snprintf(buffer, buffer_size, "%luh", (unsigned long)(seconds / 3600U));
    } else if (seconds >= 60U) {
        snprintf(buffer, buffer_size, "%lum", (unsigned long)(seconds / 60U));
    } else {
        snprintf(buffer, buffer_size, "%lus", (unsigned long)seconds);
    }
}

static void render_agent_status(const agent_status_t *status, const char *fallback_detail)
{
    const agent_status_t *data = status != NULL ? status : &s_agent_status;
    agent_state_t state = data->valid ? data->state : AGENT_STATE_UNKNOWN;
    uint32_t accent = agent_state_color(state);

    char target_text[72];
    char elapsed_text[12];
    if (data->valid && (data->agent[0] != '\0' || data->project[0] != '\0')) {
        if (data->agent[0] != '\0' && data->project[0] != '\0') {
            /* ASCII only: the Montserrat build carries no U+00B7, so a middle
             * dot separator renders as a missing-glyph box. */
            snprintf(target_text, sizeof(target_text), "%s - %s", data->agent, data->project);
        } else {
            snprintf(target_text, sizeof(target_text), "%s",
                     data->agent[0] != '\0' ? data->agent : data->project);
        }
    } else {
        target_text[0] = '\0';
    }
    if (data->valid) {
        format_elapsed_short(data->elapsed_s, elapsed_text, sizeof(elapsed_text));
    } else {
        snprintf(elapsed_text, sizeof(elapsed_text), "--");
    }

    const char *detail_text = fallback_detail;
    if (detail_text == NULL) {
        detail_text = (data->valid && data->detail[0] != '\0') ? data->detail : "";
    }

    if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }

    lv_obj_set_style_arc_color(s_agent_ui.ring, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_agent_ui.state_label, lv_color_hex(accent), 0);

    set_label_text_if_changed(s_agent_ui.state_label, agent_state_text(state));
    set_label_text_if_changed(s_agent_ui.target_label, target_text);
    set_label_text_if_changed(s_agent_ui.detail_label, detail_text);
    set_label_text_if_changed(s_agent_ui.codex_value_label,
                              agent_state_short(data->valid ? data->codex_state
                                                            : AGENT_STATE_UNKNOWN));
    set_label_text_if_changed(s_agent_ui.claude_value_label,
                              agent_state_short(data->valid ? data->claude_state
                                                            : AGENT_STATE_UNKNOWN));
    set_label_text_if_changed(s_agent_ui.elapsed_value_label, elapsed_text);
    set_label_text_if_changed(s_agent_ui.updated_label, data->valid ? data->updated : "");
    agent_refresh_lamp();
    update_status_bar_locked();

    bsp_display_unlock();
}

static void render_agent_detail(const char *detail_text)
{
    render_agent_status(&s_agent_status, detail_text);
}

static bool parse_agent_status_json(const char *json, agent_status_t *status)
{
    agent_status_t parsed = {
        .state = AGENT_STATE_UNKNOWN,
        .codex_state = AGENT_STATE_UNKNOWN,
        .claude_state = AGENT_STATE_UNKNOWN,
        .elapsed_s = 0,
        .valid = false,
    };

    char text[32];
    if (!json_get_string(json, "state", text, sizeof(text))) {
        return false;
    }
    parsed.state = agent_state_from_text(text);

    if (json_get_string(json, "codex", text, sizeof(text))) {
        parsed.codex_state = agent_state_from_text(text);
    }
    if (json_get_string(json, "claude", text, sizeof(text))) {
        parsed.claude_state = agent_state_from_text(text);
    }

    (void)json_get_string(json, "agent", parsed.agent, sizeof(parsed.agent));
    (void)json_get_string(json, "project", parsed.project, sizeof(parsed.project));
    (void)json_get_string(json, "detail", parsed.detail, sizeof(parsed.detail));
    (void)json_get_string(json, "updated", parsed.updated, sizeof(parsed.updated));

    uint64_t elapsed = 0;
    if (json_get_u64(json, "elapsed", &elapsed)) {
        parsed.elapsed_s = (uint32_t)elapsed;
    }

    parsed.valid = true;
    *status = parsed;
    return true;
}

static esp_err_t fetch_agent_status(agent_status_t *status)
{
    http_response_t *response = calloc(1, sizeof(*response));
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = http_get_url(AGENT_STATUS_URL, AGENT_HTTP_TIMEOUT_MS, false, response);
    if (ret != ESP_OK) {
        free(response);
        return ret;
    }

    if (!parse_agent_status_json(response->body, status)) {
        ESP_LOGW(TAG, "Agent status parse failed: %s", response->body);
        free(response);
        return ESP_FAIL;
    }

    free(response);
    return ESP_OK;
}

static void IRAM_ATTR reset_button_isr(void *arg)
{
    (void)arg;
    s_reset_requested = true;
}

static void init_reset_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        return;
    }

    ret = gpio_isr_handler_add(RESET_BUTTON_GPIO, reset_button_isr, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to add reset button ISR: %s", esp_err_to_name(ret));
    }
}

static esp_err_t init_qmi8658(qmi8658_dev_t **out_dev)
{
    i2c_master_bus_handle_t bus_handle = bsp_i2c_get_handle();
    if (bus_handle == NULL) {
        return ESP_FAIL;
    }

    qmi8658_dev_t *dev = calloc(1, sizeof(qmi8658_dev_t));
    if (dev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = qmi8658_init(dev, bus_handle, QMI8658_ADDRESS_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 init failed: %s", esp_err_to_name(ret));
        free(dev);
        return ret;
    }

    qmi8658_set_accel_unit_mps2(dev, true);
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_range(dev, QMI8658_ACCEL_RANGE_8G), TAG,
                        "set accel range failed");
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_odr(dev, QMI8658_ACCEL_ODR_125HZ), TAG,
                        "set accel odr failed");
    ESP_RETURN_ON_ERROR(qmi8658_enable_sensors(dev, QMI8658_ENABLE_ACCEL), TAG,
                        "enable accel failed");

    *out_dev = dev;
    return ESP_OK;
}

static bool pedometer_process_sample(pedometer_filter_t *filter, float ax, float ay, float az)
{
    float mag_g = sqrtf((ax * ax) + (ay * ay) + (az * az)) / ONE_G;
    filter->gravity_g = (filter->gravity_g * (1.0f - GRAVITY_SMOOTHING_ALPHA)) +
                        (mag_g * GRAVITY_SMOOTHING_ALPHA);

    float dynamic_g = mag_g - filter->gravity_g;
    uint32_t current_ms = now_ms();

    if (filter->armed && dynamic_g > STEP_PEAK_THRESHOLD_G &&
        (current_ms - filter->last_step_ms) > STEP_MIN_INTERVAL_MS) {
        filter->armed = false;
        filter->last_step_ms = current_ms;
        return true;
    }

    if (!filter->armed && dynamic_g < STEP_RESET_THRESHOLD_G) {
        filter->armed = true;
    }

    return false;
}

static int motion_to_mg(float ax, float ay, float az, float gravity_g)
{
    float mag_g = sqrtf((ax * ax) + (ay * ay) + (az * az)) / ONE_G;
    float dynamic_g = fabsf(mag_g - gravity_g);
    int motion_mg = (int)(dynamic_g * 1000.0f);
    return motion_mg > 999 ? 999 : motion_mg;
}

static const char *json_find_value(const char *json, const char *key)
{
    char pattern[48];
    int written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written <= 0 || written >= (int)sizeof(pattern)) {
        return NULL;
    }

    const char *position = strstr(json, pattern);
    if (position == NULL) {
        return NULL;
    }

    position = strchr(position + written, ':');
    if (position == NULL) {
        return NULL;
    }

    position++;
    while (*position != '\0' && isspace((unsigned char)*position)) {
        position++;
    }
 
    return position;
}

static bool json_get_u64(const char *json, const char *key, uint64_t *out_value)
{
    const char *position = json_find_value(json, key);
    if (position == NULL) {
        return false;
    }

    char *end = NULL;
    unsigned long long value = strtoull(position, &end, 10);
    if (end == position) {
        return false;
    }

    *out_value = (uint64_t)value;
    return true;
}

static bool json_get_string(const char *json, const char *key, char *out_value, size_t out_size)
{
    const char *position = json_find_value(json, key);
    if (position == NULL || *position != '"' || out_size == 0) {
        return false;
    }

    position++;
    size_t out_index = 0;
    while (*position != '\0' && *position != '"') {
        if (*position == '\\' && position[1] != '\0') {
            position++;
        }
        if (out_index + 1 < out_size) {
            out_value[out_index++] = *position;
        }
        position++;
    }

    out_value[out_index] = '\0';
    return *position == '"';
}

static bool json_get_string_after(const char *json, const char *anchor,
                                  const char *key, char *out_value, size_t out_size)
{
    char pattern[48];
    int written = snprintf(pattern, sizeof(pattern), "\"%s\"", anchor);
    if (written <= 0 || written >= (int)sizeof(pattern)) {
        return false;
    }

    const char *start = strstr(json, pattern);
    if (start == NULL) {
        return false;
    }
    return json_get_string(start, key, out_value, out_size);
}

static bool text_contains(const char *text, const char *needle)
{
    return text != NULL && needle != NULL && strstr(text, needle) != NULL;
}

static int weather_code_from_china_type(const char *type)
{
    if (text_contains(type, "雷")) {
        return 95;
    }
    if (text_contains(type, "雪")) {
        return 71;
    }
    if (text_contains(type, "雨")) {
        return 61;
    }
    if (text_contains(type, "雾") || text_contains(type, "霾")) {
        return 45;
    }
    if (text_contains(type, "阴")) {
        return 3;
    }
    if (text_contains(type, "云")) {
        return 2;
    }
    if (text_contains(type, "晴")) {
        return 0;
    }
    return 2;
}

static bool parse_weather_temp(const char *text, int *out_value)
{
    if (text == NULL || out_value == NULL) {
        return false;
    }

    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text) {
        return false;
    }
    *out_value = (int)value;
    return true;
}

static bool has_non_ascii(const char *text)
{
    while (text != NULL && *text != '\0') {
        if ((unsigned char)*text >= 0x80) {
            return true;
        }
        text++;
    }
    return false;
}

static void build_weather_query_city(const char *city, char *out, size_t out_size)
{
    copy_string(out, out_size, city != NULL && city[0] != '\0' ? city : WEATHER_CITY);
    if (has_non_ascii(out) && strstr(out, "市") == NULL &&
        strlen(out) + strlen("市") + 1 < out_size) {
        strcat(out, "市");
    }
}

static bool parse_codex_usage_json(const char *json, codex_usage_t *usage)
{
    codex_usage_t parsed = {
        .label = "Codex usage",
        .used_tokens = 0,
        .limit_tokens = 0,
        .updated = "unknown",
    };

    (void)json_get_string(json, "label", parsed.label, sizeof(parsed.label));
    (void)json_get_string(json, "updated", parsed.updated, sizeof(parsed.updated));

    bool have_used = json_get_u64(json, "used_tokens", &parsed.used_tokens);
    bool have_limit = json_get_u64(json, "limit_tokens", &parsed.limit_tokens);
    if (!have_used || !have_limit) {
        return false;
    }

    *usage = parsed;
    return true;
}

/* The bundled CJK subset lacks 东 and 风, so wind directions are rendered as
 * compass letters rather than the Chinese names AMap returns. */
static const char *wind_direction_en(const char *cn)
{
    if (text_contains(cn, "东") && text_contains(cn, "北")) {
        return "NE";
    }
    if (text_contains(cn, "东") && text_contains(cn, "南")) {
        return "SE";
    }
    if (text_contains(cn, "西") && text_contains(cn, "北")) {
        return "NW";
    }
    if (text_contains(cn, "西") && text_contains(cn, "南")) {
        return "SW";
    }
    if (text_contains(cn, "东")) {
        return "E";
    }
    if (text_contains(cn, "南")) {
        return "S";
    }
    if (text_contains(cn, "西")) {
        return "W";
    }
    if (text_contains(cn, "北")) {
        return "N";
    }
    return "--";
}

static bool parse_amap_weather_json(const char *json, weather_data_t *weather,
                                    const char *display_city)
{
    char status[8];
    char day_weather[32];
    char day_temp_text[16];
    char night_temp_text[16];
    char report_time[32];
    int high = 0;
    int low = 0;

    if (!json_get_string(json, "status", status, sizeof(status)) || strcmp(status, "1") != 0) {
        return false;
    }

    if (!json_get_string_after(json, "casts", "dayweather", day_weather,
                               sizeof(day_weather)) ||
        !json_get_string_after(json, "casts", "daytemp", day_temp_text,
                               sizeof(day_temp_text)) ||
        !json_get_string_after(json, "casts", "nighttemp", night_temp_text,
                               sizeof(night_temp_text))) {
        return false;
    }

    (void)json_get_string_after(json, "forecasts", "reporttime", report_time,
                                sizeof(report_time));
    if (!parse_weather_temp(day_temp_text, &high) ||
        !parse_weather_temp(night_temp_text, &low)) {
        return false;
    }

    weather->current_temp_c = high;
    weather->max_temp_c = high;
    weather->min_temp_c = low;
    weather->weather_code = weather_code_from_china_type(day_weather);
    copy_string(weather->condition, sizeof(weather->condition), day_weather);
    copy_string(weather->city, sizeof(weather->city),
                display_city != NULL && display_city[0] != '\0' ? display_city : "China City");
    copy_string(weather->region, sizeof(weather->region), "中国");
    if (strlen(report_time) >= 16) {
        snprintf(weather->updated, sizeof(weather->updated), "%.5s AMap", report_time + 11);
    } else {
        struct tm local_time;
        get_local_clock(&local_time);
        snprintf(weather->updated, sizeof(weather->updated), "%02d:%02d AMap",
                 local_time.tm_hour, local_time.tm_min);
    }
    /* Walk the casts array for the forecast detail page. Each entry is
     * scanned from the previous one's position, so per-day keys resolve to
     * the right day instead of always hitting the first match. */
    weather->day_count = 0;
    const char *cursor = strstr(json, "\"casts\"");
    for (int i = 0; cursor != NULL && i < WEATHER_FORECAST_DAYS; i++) {
        const char *entry = strstr(cursor, "\"date\"");
        if (entry == NULL) {
            break;
        }

        weather_day_t *day = &weather->days[weather->day_count];
        char field[32];
        int day_temp = 0;
        int night_temp = 0;

        if (!json_get_string(entry, "daytemp", field, sizeof(field)) ||
            !parse_weather_temp(field, &day_temp)) {
            break;
        }
        if (!json_get_string(entry, "nighttemp", field, sizeof(field)) ||
            !parse_weather_temp(field, &night_temp)) {
            break;
        }
        day->day_temp_c = day_temp;
        day->night_temp_c = night_temp;

        if (json_get_string(entry, "date", field, sizeof(field)) && strlen(field) >= 10) {
            /* "2026-07-23" -> "07-23" */
            snprintf(day->date, sizeof(day->date), "%.5s", field + 5);
        } else {
            day->date[0] = '\0';
        }

        if (json_get_string(entry, "dayweather", field, sizeof(field))) {
            copy_string(day->day_cond, sizeof(day->day_cond), field);
            day->day_code = weather_code_from_china_type(field);
        }
        if (json_get_string(entry, "nightweather", field, sizeof(field))) {
            day->night_code = weather_code_from_china_type(field);
        }

        if (json_get_string(entry, "daywind", field, sizeof(field))) {
            copy_string(day->wind_dir, sizeof(day->wind_dir), wind_direction_en(field));
        }
        if (json_get_string(entry, "daypower", field, sizeof(field))) {
            copy_string(day->wind_power, sizeof(day->wind_power), field);
        }

        day->valid = true;
        weather->day_count++;
        cursor = entry + 6;
    }

    weather->valid = true;
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_t *response = (http_response_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && response != NULL && event->data_len > 0) {
        int remaining = (int)sizeof(response->body) - response->length - 1;
        int copy_len = event->data_len < remaining ? event->data_len : remaining;
        if (copy_len > 0) {
            memcpy(response->body + response->length, event->data, (size_t)copy_len);
            response->length += copy_len;
            response->body[response->length] = '\0';
        }
    }
    return ESP_OK;
}

static bool http_headers_have_chunked(const char *headers)
{
    for (const char *p = headers; *p != '\0'; p++) {
        if (tolower((unsigned char)p[0]) == 'c' && tolower((unsigned char)p[1]) == 'h' &&
            tolower((unsigned char)p[2]) == 'u' && tolower((unsigned char)p[3]) == 'n' &&
            tolower((unsigned char)p[4]) == 'k' && tolower((unsigned char)p[5]) == 'e' &&
            tolower((unsigned char)p[6]) == 'd') {
            return true;
        }
    }
    return false;
}

/* Collapse a chunked transfer-encoded body in place: strip the hex size
 * lines and trailing CRLFs so only payload bytes remain. */
static bool dechunk_http_body(char *body, int *length)
{
    int total = *length;
    int out = 0;
    int in = 0;

    while (in < total) {
        char *size_start = body + in;
        char *size_end = NULL;
        long chunk_len = strtol(size_start, &size_end, 16);
        if (size_end == size_start || chunk_len < 0) {
            return false;
        }

        char *data_start = strstr(size_end, "\r\n");
        if (data_start == NULL) {
            return false;
        }
        data_start += 2;

        if (chunk_len == 0) {
            break;
        }

        int data_offset = (int)(data_start - body);
        if (data_offset + chunk_len > total) {
            chunk_len = total - data_offset;
        }
        memmove(body + out, data_start, (size_t)chunk_len);
        out += (int)chunk_len;
        in = data_offset + (int)chunk_len + 2;
    }

    body[out] = '\0';
    *length = out;
    return true;
}

static esp_err_t http_get_url(const char *url, int timeout_ms, bool use_crt_bundle,
                              http_response_t *response)
{
    (void)use_crt_bundle;
    memset(response, 0, sizeof(*response));

    const char *scheme = "http://";
    size_t scheme_len = strlen(scheme);
    if (strncmp(url, scheme, scheme_len) != 0) {
        ESP_LOGW(TAG, "Only plain HTTP is supported by raw weather client");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *host_start = url + scheme_len;
    const char *path_start = strchr(host_start, '/');
    if (path_start == NULL) {
        path_start = "/";
    }

    const char *host_end = path_start;
    const char *port_start = memchr(host_start, ':', (size_t)(host_end - host_start));
    size_t host_len = (size_t)((port_start != NULL ? port_start : host_end) - host_start);
    if (host_len == 0 || host_len >= 96) {
        return ESP_ERR_INVALID_ARG;
    }

    char host[96];
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    char port_text[8] = "80";
    if (port_start != NULL) {
        size_t port_len = (size_t)(host_end - port_start - 1);
        if (port_len == 0 || port_len >= sizeof(port_text)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(port_text, port_start + 1, port_len);
        port_text[port_len] = '\0';
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    ESP_LOGI(TAG, "Resolving weather host %s", host);
    int gai_ret = getaddrinfo(host, port_text, &hints, &result);
    if (gai_ret != 0 || result == NULL) {
        ESP_LOGW(TAG, "Weather DNS failed for %s: %d", host, gai_ret);
        return ESP_FAIL;
    }

    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(result);
        ESP_LOGW(TAG, "Weather socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Connecting weather host %s:%s", host, port_text);
    if (connect(sock, result->ai_addr, result->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "Weather connect failed: errno=%d", errno);
        close(sock);
        freeaddrinfo(result);
        return ESP_FAIL;
    }
    freeaddrinfo(result);

    /* HTTP/1.0 so the server may not use chunked transfer encoding; this raw
     * client reads the body as-is. A dechunk fallback still runs below in
     * case the server chunks anyway. */
    char request[768];
    int request_len = snprintf(request, sizeof(request),
                               "GET %s HTTP/1.0\r\n"
                               "Host: %s\r\n"
                               "User-Agent: CodexPedometer/1.0\r\n"
                               "Connection: close\r\n"
                               "\r\n",
                               path_start, host);
    if (request_len <= 0 || request_len >= (int)sizeof(request)) {
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    int sent_total = 0;
    while (sent_total < request_len) {
        int sent = send(sock, request + sent_total, request_len - sent_total, 0);
        if (sent <= 0) {
            ESP_LOGW(TAG, "Weather send failed: errno=%d", errno);
            close(sock);
            return ESP_FAIL;
        }
        sent_total += sent;
    }

    while (response->length < (int)sizeof(response->body) - 1) {
        int room = (int)sizeof(response->body) - response->length - 1;
        int received = recv(sock, response->body + response->length, room, 0);
        if (received > 0) {
            response->length += received;
            response->body[response->length] = '\0';
            continue;
        }
        if (received == 0) {
            break;
        }
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            break;
        }
        ESP_LOGW(TAG, "Weather recv failed: errno=%d", errno);
        close(sock);
        return ESP_FAIL;
    }
    close(sock);

    if (response->length == 0) {
        ESP_LOGW(TAG, "Weather HTTP timeout/no data");
        return ESP_ERR_TIMEOUT;
    }

    int status_code = 0;
    if (sscanf(response->body, "HTTP/%*s %d", &status_code) != 1) {
        ESP_LOGW(TAG, "Weather HTTP response missing status line");
        return ESP_FAIL;
    }

    char *body = strstr(response->body, "\r\n\r\n");
    if (body == NULL) {
        ESP_LOGW(TAG, "Weather HTTP response missing body");
        return ESP_FAIL;
    }
    *body = '\0';
    bool chunked = http_headers_have_chunked(response->body);
    *body = '\r';
    body += 4;
    size_t header_len = (size_t)(body - response->body);
    size_t body_len = (size_t)response->length - header_len;
    memmove(response->body, body, body_len);
    response->body[body_len] = '\0';
    response->length = (int)body_len;

    if (chunked && !dechunk_http_body(response->body, &response->length)) {
        ESP_LOGW(TAG, "Weather HTTP dechunk failed");
        return ESP_FAIL;
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "Weather HTTP status: %d from %s", status_code, host);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Weather HTTP OK, %d bytes", response->length);
    return ESP_OK;
}

static esp_err_t fetch_weather(weather_data_t *weather)
{
    char query_city[WEATHER_CITY_STORAGE_SIZE];
    char display_city[WEATHER_CITY_STORAGE_SIZE];
    char adcode[16];
    char url[512];
    http_response_t *response = NULL;

    if (!weather_api_key_configured()) {
        return ESP_ERR_INVALID_STATE;
    }

    response = calloc(1, sizeof(*response));
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    build_weather_query_city(s_weather_city, query_city, sizeof(query_city));
    (void)lookup_weather_city_code(query_city, adcode, sizeof(adcode),
                                   display_city, sizeof(display_city));

    weather_data_t parsed = {
        .weather_code = 0,
        .current_temp_c = 0,
        .min_temp_c = 0,
        .max_temp_c = 0,
        .valid = false,
    };

    render_weather_status("AMap HTTP");
    ESP_LOGI(TAG, "Fetching AMap weather for %s (%s)", display_city, adcode);
    snprintf(url, sizeof(url),
             "http://restapi.amap.com/v3/weather/weatherInfo?city=%s&key=%s&extensions=all&output=JSON",
             adcode, s_weather_api_key);
    esp_err_t ret = http_get_url(url, WEATHER_HTTP_TIMEOUT_MS, false, response);
    if (ret != ESP_OK) {
        free(response);
        return ret;
    }

    if (!parse_amap_weather_json(response->body, &parsed, display_city)) {
        ESP_LOGW(TAG, "AMap weather parse failed: %s", response->body);
        free(response);
        return ESP_FAIL;
    }

    /* The forecast payload has no live reading, so "current" above is just
     * the daytime high. Fetch the live observation too - it drives the
     * corner ring (position of now within today's low..high) and gives the
     * actual present condition. Forecast values remain the fallback. */
    snprintf(url, sizeof(url),
             "http://restapi.amap.com/v3/weather/weatherInfo?city=%s&key=%s&extensions=base&output=JSON",
             adcode, s_weather_api_key);
    if (http_get_url(url, WEATHER_HTTP_TIMEOUT_MS, false, response) == ESP_OK) {
        char live_text[32];
        int live_temp = 0;
        if (json_get_string_after(response->body, "lives", "temperature", live_text,
                                  sizeof(live_text)) &&
            parse_weather_temp(live_text, &live_temp)) {
            parsed.current_temp_c = live_temp;
        }
        if (json_get_string_after(response->body, "lives", "weather", live_text,
                                  sizeof(live_text)) &&
            live_text[0] != '\0') {
            copy_string(parsed.condition, sizeof(parsed.condition), live_text);
            parsed.weather_code = weather_code_from_china_type(live_text);
        }
        ESP_LOGI(TAG, "Weather live %dC in range %d..%dC", parsed.current_temp_c,
                 parsed.min_temp_c, parsed.max_temp_c);
        for (int i = 0; i < parsed.day_count; i++) {
            ESP_LOGI(TAG, "  forecast[%d] %s %d..%dC day=%s night=%s wind=%s %s", i,
                     parsed.days[i].date, parsed.days[i].night_temp_c,
                     parsed.days[i].day_temp_c, weather_condition_en(parsed.days[i].day_code),
                     weather_condition_en(parsed.days[i].night_code),
                     parsed.days[i].wind_dir, parsed.days[i].wind_power);
        }
    }

    *weather = parsed;
    free(response);
    return ESP_OK;
}

static esp_err_t fetch_codex_usage(codex_usage_t *usage)
{
    http_response_t *response = calloc(1, sizeof(*response));
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = CODEX_USAGE_URL,
        .timeout_ms = CODEX_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(response);
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Codex usage request failed: %s", esp_err_to_name(ret));
        free(response);
        return ret;
    }

    if (status_code != 200) {
        ESP_LOGW(TAG, "Codex usage HTTP status: %d", status_code);
        free(response);
        return ESP_FAIL;
    }

    if (!parse_codex_usage_json(response->body, usage)) {
        ESP_LOGW(TAG, "Codex usage JSON parse failed: %s", response->body);
        free(response);
        return ESP_FAIL;
    }

    free(response);
    return ESP_OK;
}

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    s_ntp_sync_in_progress = false;
    ESP_LOGI(TAG, "NTP time synchronized");
}

static esp_err_t start_sntp_once(void)
{
    if (s_sntp_started) {
        return esp_netif_sntp_start();
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    config.sync_cb = time_sync_notification_cb;
    config.wait_for_sync = false;

    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret == ESP_ERR_INVALID_STATE) {
        s_sntp_started = true;
        return esp_netif_sntp_start();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    s_sntp_started = true;
    return ESP_OK;
}

static esp_err_t request_ntp_sync(void)
{
    if (s_time_synced) {
        return ESP_OK;
    }

    uint32_t current_ms = now_ms();
    if (s_ntp_sync_in_progress &&
        (current_ms - s_last_ntp_attempt_ms) < NTP_RETRY_INTERVAL_MS) {
        return ESP_ERR_NOT_FINISHED;
    }

    s_last_ntp_attempt_ms = current_ms;
    s_ntp_sync_in_progress = true;
    esp_err_t ret = start_sntp_once();
    if (ret != ESP_OK) {
        s_ntp_sync_in_progress = false;
    }
    return ret;
}

static esp_err_t apply_station_config(void)
{
    if (!wifi_configured()) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t sta_config = {0};
    size_t ssid_len = strnlen(s_wifi_credentials.ssid, sizeof(sta_config.sta.ssid));
    size_t pass_len = strnlen(s_wifi_credentials.password, sizeof(sta_config.sta.password));
    memcpy(sta_config.sta.ssid, s_wifi_credentials.ssid, ssid_len);
    memcpy(sta_config.sta.password, s_wifi_credentials.password, pass_len);
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    return esp_wifi_set_config(WIFI_IF_STA, &sta_config);
}

static void reconnect_station(void)
{
    if (!s_wifi_started || !wifi_configured()) {
        return;
    }

    s_wifi_retry_num = 0;
    s_time_synced = false;
    s_ntp_sync_in_progress = false;
    s_last_ntp_attempt_ms = 0;
    if (s_sntp_started) {
        esp_netif_sntp_deinit();
        s_sntp_started = false;
    }
    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    esp_err_t ret = apply_station_config();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Apply STA config failed: %s", esp_err_to_name(ret));
        render_weather_status("Wi-Fi config failed");
        return;
    }

    render_weather_status("Wi-Fi connecting");
    (void)esp_wifi_disconnect();
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "STA reconnect failed: %s", esp_err_to_name(ret));
        render_weather_status("Wi-Fi connect failed");
    }
    render_time_page();
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *value)
{
    char *src = value;
    char *dst = value;
    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && hex_value(src[1]) >= 0 && hex_value(src[2]) >= 0) {
            *dst++ = (char)((hex_value(src[1]) << 4) | hex_value(src[2]));
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool get_form_value(const char *form, const char *key, char *out, size_t out_size)
{
    if (out_size == 0) {
        return false;
    }
    out[0] = '\0';

    size_t key_len = strlen(key);
    const char *cursor = form;
    while (cursor != NULL && *cursor != '\0') {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            cursor += key_len + 1;
            const char *end = strchr(cursor, '&');
            size_t len = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
            if (len >= out_size) {
                len = out_size - 1;
            }
            memcpy(out, cursor, len);
            out[len] = '\0';
            url_decode(out);
            return true;
        }
        cursor = strchr(cursor, '&');
        if (cursor != NULL) {
            cursor++;
        }
    }
    return false;
}

static void html_escape_copy(char *dest, size_t dest_size, const char *src)
{
    if (dest_size == 0) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 1 < dest_size; i++) {
        char ch = src[i];
        if (ch == '<' || ch == '>' || ch == '&' || ch == '"' || ch == '\'') {
            ch = '_';
        }
        dest[out++] = ch;
    }
    dest[out] = '\0';
}

static esp_err_t config_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
                             "<!doctype html><html><head><meta charset='utf-8'>"
                             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                             "<title>Codex Pedometer Wi-Fi</title>"
                             "<style>body{font-family:system-ui;margin:24px;background:#0b1017;color:#eef}"
                             "label,select,input,button{display:block;width:100%;box-sizing:border-box}"
                             "select,input{margin:8px 0 16px;padding:12px;border-radius:8px;border:1px solid #456;background:#111b27;color:#fff}"
                             "button{padding:12px;border:0;border-radius:8px;background:#18d7f5;color:#001018;font-weight:700}"
                             ".card{max-width:520px;margin:auto}.hint{color:#9fb0c0;font-size:14px}</style>"
                             "</head><body><div class='card'><h2>ESP32 Wi-Fi 配网</h2>"
                             "<p class='hint'>手机连接热点 CodexPedometer 后，访问 192.168.4.1。保存后设备会用 STA 连接你选择的 Wi-Fi，AP 会继续保留。</p>"
                             "<p class='hint'>天气使用高德开放平台 Web 服务。可填城市名，也可填 6 位高德 adcode，例如青岛 370200。</p>"
                             "<form method='post' action='/save'><label>选择 Wi-Fi</label><select name='ssid'>");

    /* A blocking scan pulls the shared radio off the AP channel and can drop
     * the provisioning client, so keep the dwell short and cache the results
     * for a while: refreshing the page does not rescan. */
    uint32_t scan_now = now_ms();
    bool cache_fresh = s_wifi_scan_cache_count > 0 &&
                       (scan_now - s_wifi_scan_cache_ms) < WIFI_SCAN_CACHE_MS;
    if (!cache_fresh) {
        uint16_t ap_count = WIFI_SCAN_MAX_AP;
        wifi_scan_config_t scan_config = {
            .show_hidden = false,
            .scan_time = {
                .active = {
                    .min = 100,
                    .max = 150,
                },
            },
        };
        esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
        if (ret == ESP_OK) {
            ret = esp_wifi_scan_get_ap_records(&ap_count, s_wifi_scan_cache);
        }
        if (ret == ESP_OK && ap_count > 0) {
            s_wifi_scan_cache_count = ap_count;
            s_wifi_scan_cache_ms = scan_now;
        } else {
            ESP_LOGW(TAG, "Config page Wi-Fi scan failed: %s", esp_err_to_name(ret));
        }
    }

    if (s_wifi_scan_cache_count > 0) {
        for (uint16_t i = 0; i < s_wifi_scan_cache_count; i++) {
            char ssid[WIFI_SSID_STORAGE_SIZE];
            char escaped[WIFI_SSID_STORAGE_SIZE];
            char option[160];
            copy_string(ssid, sizeof(ssid), (const char *)s_wifi_scan_cache[i].ssid);
            html_escape_copy(escaped, sizeof(escaped), ssid);
            snprintf(option, sizeof(option), "<option value='%s'>%s (%d dBm)</option>",
                     escaped, escaped, s_wifi_scan_cache[i].rssi);
            httpd_resp_sendstr_chunk(req, option);
        }
    } else {
        httpd_resp_sendstr_chunk(req, "<option value=''>扫描失败，请刷新页面</option>");
    }

    httpd_resp_sendstr_chunk(req,
                             "</select><label>或手动输入 SSID</label>"
                             "<input name='manual_ssid' maxlength='32' placeholder='可留空，默认使用上面的选择'>"
                             "<label>Wi-Fi 密码</label><input name='password' maxlength='64' type='password'>"
                             "<label>天气城市</label>");
    char escaped_city[WEATHER_CITY_STORAGE_SIZE];
    html_escape_copy(escaped_city, sizeof(escaped_city),
                     s_weather_city[0] != '\0' ? s_weather_city : WEATHER_CITY);
    char city_input[180];
    snprintf(city_input, sizeof(city_input),
             "<input name='weather_city' maxlength='63' value='%s' placeholder='例如 青岛市、上海市、370200'>",
             escaped_city);
    httpd_resp_sendstr_chunk(req, city_input);

    char escaped_key[WEATHER_KEY_STORAGE_SIZE];
    html_escape_copy(escaped_key, sizeof(escaped_key),
                     weather_api_key_configured() ? s_weather_api_key : "");
    char key_input[240];
    snprintf(key_input, sizeof(key_input),
             "<label>高德天气 Web服务 Key</label>"
             "<input name='weather_key' maxlength='79' value='%s' placeholder='在高德开放平台申请 Web服务 Key'>",
             escaped_key);
    httpd_resp_sendstr_chunk(req, key_input);

    char goal_input[220];
    snprintf(goal_input, sizeof(goal_input),
             "<label>每日步数目标</label>"
             "<input name='step_goal' type='number' min='%lu' max='%lu' value='%lu'>",
             (unsigned long)PEDOMETER_MIN_GOAL_STEPS,
             (unsigned long)PEDOMETER_MAX_GOAL_STEPS,
             (unsigned long)s_pedometer_goal_steps);
    httpd_resp_sendstr_chunk(req, goal_input);

    httpd_resp_sendstr_chunk(req, "<label>小组件颜色</label>"
                                  "<div style='display:grid;grid-template-columns:repeat(4,1fr);"
                                  "gap:8px;margin:8px 0 16px'>");
    for (int i = 0; i < THEME_COUNT; i++) {
        char color_input[192];
        snprintf(color_input, sizeof(color_input),
                 "<span style='font-size:13px;color:#9fb0c0'>%s"
                 "<input name='theme%d' type='color' value='#%06lx'"
                 " style='margin:4px 0 0;padding:2px;height:34px'></span>",
                 THEME_NAMES[i], i, (unsigned long)s_theme_color[i]);
        httpd_resp_sendstr_chunk(req, color_input);
    }
    httpd_resp_sendstr_chunk(req, "</div>");

    httpd_resp_sendstr_chunk(req,
                             "<button type='submit'>保存并连接</button></form>"
                             "<p class='hint'>当前保存 SSID: ");
    char escaped_current[WIFI_SSID_STORAGE_SIZE];
    html_escape_copy(escaped_current, sizeof(escaped_current),
                     wifi_configured() ? s_wifi_credentials.ssid : "未配置");
    httpd_resp_sendstr_chunk(req, escaped_current);
    httpd_resp_sendstr_chunk(req, "</p></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t config_page_post_handler(httpd_req_t *req)
{
    size_t body_size = 1024;
    char *body = calloc(1, body_size);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Out of memory");
        return ESP_OK;
    }
    int total = 0;
    int remaining = req->content_len;
    while (remaining > 0 && total < (int)body_size - 1) {
        int recv_len = httpd_req_recv(req, body + total,
                                      remaining < (int)body_size - 1 - total
                                          ? remaining
                                          : (int)body_size - 1 - total);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            return ESP_FAIL;
        }
        total += recv_len;
        remaining -= recv_len;
    }
    body[total] = '\0';

    char ssid[WIFI_SSID_STORAGE_SIZE];
    char manual_ssid[WIFI_SSID_STORAGE_SIZE];
    char password[WIFI_PASSWORD_STORAGE_SIZE];
    char weather_city[WEATHER_CITY_STORAGE_SIZE];
    char weather_key[WEATHER_KEY_STORAGE_SIZE];
    char step_goal_text[16];
    (void)get_form_value(body, "ssid", ssid, sizeof(ssid));
    (void)get_form_value(body, "manual_ssid", manual_ssid, sizeof(manual_ssid));
    (void)get_form_value(body, "password", password, sizeof(password));
    (void)get_form_value(body, "weather_city", weather_city, sizeof(weather_city));
    (void)get_form_value(body, "weather_key", weather_key, sizeof(weather_key));
    (void)get_form_value(body, "step_goal", step_goal_text, sizeof(step_goal_text));
    if (manual_ssid[0] != '\0') {
        copy_string(ssid, sizeof(ssid), manual_ssid);
    }

    esp_err_t ret = ESP_OK;
    bool wifi_changed = ssid[0] != '\0';
    if (wifi_changed) {
        ret = save_wifi_credentials(ssid, password);
    }
    bool weather_changed = false;
    if (ret == ESP_OK && weather_city[0] != '\0') {
        ret = save_weather_city(weather_city);
        weather_changed = ret == ESP_OK;
    }
    if (ret == ESP_OK && weather_key[0] != '\0') {
        ret = save_weather_api_key(weather_key);
        weather_changed = ret == ESP_OK;
    }
    if (ret == ESP_OK && step_goal_text[0] != '\0') {
        char *end = NULL;
        unsigned long parsed_goal = strtoul(step_goal_text, &end, 10);
        if (end != step_goal_text && *end == '\0' &&
            parsed_goal >= PEDOMETER_MIN_GOAL_STEPS &&
            parsed_goal <= PEDOMETER_MAX_GOAL_STEPS) {
            ret = save_pedometer_goal((uint32_t)parsed_goal);
        } else {
            ret = ESP_ERR_INVALID_ARG;
        }
    }

    /* Ring colors: <input type=color> posts "#rrggbb" (# arrives as %23 and
     * url_decode already unescaped it). */
    bool theme_changed = false;
    for (int i = 0; i < THEME_COUNT; i++) {
        char field[12];
        char color_text[12];
        snprintf(field, sizeof(field), "theme%d", i);
        if (!get_form_value(body, field, color_text, sizeof(color_text))) {
            continue;
        }
        const char *hex = color_text[0] == '#' ? color_text + 1 : color_text;
        char *end = NULL;
        unsigned long color = strtoul(hex, &end, 16);
        if (end != hex && *end == '\0' && color <= 0xFFFFFF &&
            (uint32_t)color != s_theme_color[i]) {
            s_theme_color[i] = (uint32_t)color;
            theme_changed = true;
        }
    }
    if (theme_changed) {
        save_ui_settings();
        apply_theme_colors();
    }
    if (ret == ESP_OK && weather_changed) {
        s_last_weather.valid = false;
        copy_string(s_last_weather.condition, sizeof(s_last_weather.condition), "等待天气");
        copy_string(s_last_weather.updated, sizeof(s_last_weather.updated), "weather saved");
        render_weather_status("Weather saved");
        request_weather_refresh();
    }

    if (ret == ESP_OK) {
        if (wifi_changed) {
            reconnect_station();
        }
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req,
                           "<!doctype html><html><head><meta charset='utf-8'>"
                           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                           "</head><body style='font-family:system-ui;background:#0b1017;color:#eef'>"
                           "<h2>已保存，正在连接...</h2><p>可以回到设备屏幕查看 WiFi 状态。</p>"
                           "<p><a href='/' style='color:#18d7f5'>返回配网页</a></p></body></html>");
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Save failed");
    }
    free(body);
    return ESP_OK;
}

static esp_err_t start_config_web_server(void)
{
    if (s_config_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.stack_size = 4096;
    esp_err_t ret = httpd_start(&s_config_server, &config);
    if (ret != ESP_OK) {
        return ret;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = config_page_get_handler,
    };
    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = config_page_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_config_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_config_server, &save_uri));
    ESP_LOGI(TAG, "Config web server started at http://192.168.4.1");
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_client_count++;
        ESP_LOGI(TAG, "Setup AP client joined (%d); STA retries paused", s_ap_client_count);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_client_count > 0) {
            s_ap_client_count--;
        }
        ESP_LOGI(TAG, "Setup AP client left (%d)", s_ap_client_count);
        if (s_ap_client_count == 0 && wifi_configured() && s_wifi_event_group != NULL &&
            (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) == 0) {
            s_wifi_retry_num = 0;
            xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (wifi_configured() && s_ap_client_count == 0) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            xEventGroupSetBits(s_wifi_event_group, WEATHER_REFRESH_REQUEST_BIT);
        }
        render_weather_status("Wi-Fi reconnecting");
        if (s_ap_client_count > 0) {
            /* A phone is provisioning on the setup AP: keep the radio parked
             * on the AP channel instead of scanning for the router. */
            if (s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else if (wifi_configured() && s_wifi_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGI(TAG, "Retry Wi-Fi connection: %d", s_wifi_retry_num);
        } else if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_num = 0;
        s_ntp_sync_in_progress = false;
        s_last_ntp_attempt_ms = 0;
        if (s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT |
                                                  WEATHER_REFRESH_REQUEST_BIT);
        }
        /* Kick SNTP right away instead of waiting for the codex task loop. */
        (void)request_ntp_sync();
        render_weather_status("Weather updating");
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static esp_err_t start_wifi_station(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (ap_netif == NULL || sta_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register wifi event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register ip event failed");

    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", WIFI_SETUP_AP_SSID);
    snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s",
             WIFI_SETUP_AP_PASSWORD);
    ap_config.ap.ssid_len = strlen(WIFI_SETUP_AP_SSID);
    ap_config.ap.channel = WIFI_SETUP_AP_CHANNEL;
    ap_config.ap.max_connection = WIFI_SETUP_AP_MAX_CONN;
    ap_config.ap.authmode = strlen(WIFI_SETUP_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA_WPA2_PSK
                                                                 : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG,
                        "set AP config failed");
    if (wifi_configured()) {
        ESP_RETURN_ON_ERROR(apply_station_config(), TAG, "set STA config failed");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi failed");
    /* Modem power save makes softAP beaconing irregular; keep it off so the
     * setup hotspot stays discoverable. */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    s_wifi_started = true;
    ESP_RETURN_ON_ERROR(start_config_web_server(), TAG, "start config web failed");
    render_time_page();
    render_weather_status(wifi_configured() ? "Wi-Fi connecting" : "Open setup AP");
    render_codex_status(wifi_configured() ? "Wi-Fi connecting" : "Open setup AP");
    return ESP_OK;
}

/* In-place radix-2 FFT; 512 points at ~31 fps is light work for the S3, so
 * no DSP-library dependency is needed. */
static void music_fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j |= bit;
        if (i < j) {
            float tmp = re[i];
            re[i] = re[j];
            re[j] = tmp;
            tmp = im[i];
            im[i] = im[j];
            im[j] = tmp;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang);
        float wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f;
            float ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                float ur = re[i + k];
                float ui_ = im[i + k];
                re[i + k] = ur + vr;
                im[i + k] = ui_ + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui_ - vi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

static void render_music_bars_locked(const float *levels, const float *peaks)
{
    int32_t start_x = (BSP_LCD_H_RES - (MUSIC_BANDS * MUSIC_BAR_PITCH - 5)) / 2;
    for (int i = 0; i < MUSIC_BANDS; i++) {
        int32_t h = (int32_t)(levels[i] * MUSIC_BAR_MAX_H);
        if (h < 4) {
            h = 4;
        }
        lv_obj_set_pos(s_music_ui.bars[i], start_x + i * MUSIC_BAR_PITCH,
                       MUSIC_BASELINE_Y - h);
        lv_obj_set_height(s_music_ui.bars[i], h);

        int32_t p = (int32_t)(peaks[i] * MUSIC_BAR_MAX_H);
        if (p < h) {
            p = h;
        }
        lv_obj_set_y(s_music_ui.peaks[i], MUSIC_BASELINE_Y - p - 8);
    }
}

static void music_audio_task(void *arg)
{
    (void)arg;

    esp_codec_dev_handle_t mic = NULL;
    bool mic_open = false;
    int band_edge[MUSIC_BANDS + 1];
    float bar_level[MUSIC_BANDS] = {0};
    float bar_peak[MUSIC_BANDS] = {0};
    float agc_ref = 1000.0f;
    uint32_t frame = 0;

    /* Log-spaced band edges from ~62 Hz to ~7.5 kHz (bin = 31.25 Hz). */
    for (int i = 0; i <= MUSIC_BANDS; i++) {
        band_edge[i] = (int)lroundf(2.0f * powf(120.0f, (float)i / MUSIC_BANDS));
    }

    static int16_t samples[MUSIC_FFT_SIZE];
    float *re = heap_caps_malloc(MUSIC_FFT_SIZE * sizeof(float),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float *im = heap_caps_malloc(MUSIC_FFT_SIZE * sizeof(float),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float *hann = heap_caps_malloc(MUSIC_FFT_SIZE * sizeof(float),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (re == NULL || im == NULL || hann == NULL) {
        ESP_LOGE(TAG, "Music FFT buffers alloc failed");
        vTaskDelete(NULL);
        return;
    }
    for (int i = 0; i < MUSIC_FFT_SIZE; i++) {
        hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (MUSIC_FFT_SIZE - 1)));
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = MUSIC_SAMPLE_RATE,
        .mclk_multiple = 0,
    };

    /* One boot-time capture proves the mic path end to end in the serial
     * log, since the page itself can only be exercised by touch. */
    mic = bsp_audio_codec_microphone_init();
    if (mic == NULL) {
        ESP_LOGE(TAG, "Microphone codec init failed");
        vTaskDelete(NULL);
        return;
    }
    if (esp_codec_dev_open(mic, &fs) == 0) {
        (void)esp_codec_dev_set_in_gain(mic, MUSIC_MIC_GAIN_DB);
        /* The first blocks after open are zero until DMA fills; sample the
         * fifth so the log shows real room noise. */
        int rms = -1;
        for (int block = 0; block < 5; block++) {
            if (esp_codec_dev_read(mic, samples, sizeof(samples)) != 0) {
                rms = -1;
                break;
            }
            int64_t acc = 0;
            for (int i = 0; i < MUSIC_FFT_SIZE; i++) {
                acc += (int32_t)samples[i] * samples[i];
            }
            rms = (int)sqrtf((float)(acc / MUSIC_FFT_SIZE));
        }
        if (rms >= 0) {
            ESP_LOGI(TAG, "Mic self-test OK, RMS=%d", rms);
        } else {
            ESP_LOGW(TAG, "Mic self-test read failed");
        }
        esp_codec_dev_close(mic);
    } else {
        ESP_LOGW(TAG, "Mic self-test open failed");
    }

    while (true) {
        if (s_current_page != APP_PAGE_MUSIC) {
            if (mic_open) {
                esp_codec_dev_close(mic);
                mic_open = false;
            }
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        if (!mic_open) {
            if (esp_codec_dev_open(mic, &fs) != 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            (void)esp_codec_dev_set_in_gain(mic, MUSIC_MIC_GAIN_DB);
            mic_open = true;
        }

        if (esp_codec_dev_read(mic, samples, sizeof(samples)) != 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int64_t acc = 0;
        for (int i = 0; i < MUSIC_FFT_SIZE; i++) {
            acc += (int32_t)samples[i] * samples[i];
            re[i] = (float)samples[i] * hann[i];
            im[i] = 0.0f;
        }
        float rms = sqrtf((float)(acc / MUSIC_FFT_SIZE));
        music_fft(re, im, MUSIC_FFT_SIZE);

        float band_mag[MUSIC_BANDS];
        float band_max = 0.0f;
        for (int b = 0; b < MUSIC_BANDS; b++) {
            float sum = 0.0f;
            int lo = band_edge[b];
            int hi = band_edge[b + 1];
            if (hi <= lo) {
                hi = lo + 1;
            }
            for (int k = lo; k < hi; k++) {
                sum += sqrtf(re[k] * re[k] + im[k] * im[k]);
            }
            band_mag[b] = sum / (float)(hi - lo);
            if (band_mag[b] > band_max) {
                band_max = band_mag[b];
            }
        }

        /* Slow-release AGC keeps the display lively at any volume. */
        agc_ref *= 0.995f;
        if (band_max > agc_ref) {
            agc_ref = band_max;
        }
        if (agc_ref < 400.0f) {
            agc_ref = 400.0f;
        }

        for (int b = 0; b < MUSIC_BANDS; b++) {
            float level = powf(band_mag[b] / agc_ref, 0.6f);
            if (level > 1.0f) {
                level = 1.0f;
            }
            bar_level[b] = level > bar_level[b] ? level : bar_level[b] * 0.78f;
            float fall = bar_peak[b] - 0.02f;
            bar_peak[b] = bar_level[b] > fall ? bar_level[b] : fall;
        }

        if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) == ESP_OK) {
            render_music_bars_locked(bar_level, bar_peak);

            if ((frame++ % MUSIC_SLOW_UI_DIVIDER) == 0) {
                float db = 20.0f * log10f(rms / 32768.0f + 1e-6f);
                int vu = (int)((db + 60.0f) * (100.0f / 60.0f));
                vu = clamp_int(vu, 0, 100);
                if (lv_arc_get_value(s_music_ui.vu_arc) != vu) {
                    lv_arc_set_value(s_music_ui.vu_arc, vu);
                }

                float bass = 0.0f;
                float treb = 0.0f;
                for (int b = 0; b < 6; b++) {
                    bass += bar_level[b];
                }
                for (int b = MUSIC_BANDS - 6; b < MUSIC_BANDS; b++) {
                    treb += bar_level[b];
                }
                char text[16];
                snprintf(text, sizeof(text), "%d", (int)(bass * 100.0f / 6.0f));
                set_label_text_if_changed(s_music_ui.bass_value_label, text);
                snprintf(text, sizeof(text), "%d", (int)db);
                set_label_text_if_changed(s_music_ui.vol_value_label, text);
                snprintf(text, sizeof(text), "%d", (int)(treb * 100.0f / 6.0f));
                set_label_text_if_changed(s_music_ui.treb_value_label, text);
            }
            bsp_display_unlock();
        }
    }
}

static void pedometer_task(void *arg)
{
    qmi8658_dev_t *dev = (qmi8658_dev_t *)arg;
    pedometer_filter_t filter = {
        .gravity_g = 1.0f,
        .armed = true,
        .last_step_ms = 0,
    };
    uint32_t last_ui_ms = 0;
    uint32_t temp_tick = 0;

    while (true) {
        if ((temp_tick++ % TEMP_POLL_TICKS) == 0) {
            float temp_c = 0.0f;
            if (qmi8658_read_temp(dev, &temp_c) == ESP_OK) {
                s_board_temp_c = temp_c;
            }
        }

        if (s_reset_requested) {
            s_reset_requested = false;
            s_step_count = 0;
            filter.armed = true;
            filter.last_step_ms = 0;
            render_pedometer(s_step_count, 0, "Reset by BOOT");
        }

        bool ready = true;
        esp_err_t ret = qmi8658_is_data_ready(dev, &ready);
        if (ret == ESP_OK && ready) {
            float ax = 0.0f;
            float ay = 0.0f;
            float az = 0.0f;
            ret = qmi8658_read_accel(dev, &ax, &ay, &az);
            if (ret == ESP_OK) {
                if (pedometer_process_sample(&filter, ax, ay, az)) {
                    s_step_count++;
                }

                uint32_t current_ms = now_ms();
                if ((current_ms - last_ui_ms) >= UI_REFRESH_MS) {
                    last_ui_ms = current_ms;
                    render_pedometer(s_step_count, motion_to_mg(ax, ay, az, filter.gravity_g),
                                     "Tracking steps");
                }
            } else if ((now_ms() - last_ui_ms) >= UI_REFRESH_MS) {
                last_ui_ms = now_ms();
                render_pedometer(s_step_count, 0, "QMI8658 read failed");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PEDOMETER_TASK_DELAY_MS));
    }
}

static void codex_usage_task(void *arg)
{
    (void)arg;

    esp_err_t ret = start_wifi_station();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi start failed: %s", esp_err_to_name(ret));
        render_codex_status("Wi-Fi start failed");
        render_time_page();
        vTaskDelete(NULL);
        return;
    }

    uint32_t last_codex_fetch_ms = 0;
    bool codex_fetched_once = false;

    while (true) {
        if (!wifi_configured()) {
            render_codex_status("Open setup AP");
            render_time_page();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
                                               pdFALSE, pdMS_TO_TICKS(30000));
        if ((bits & WIFI_CONNECTED_BIT) != 0) {
            if (!s_time_synced) {
                ret = request_ntp_sync();
                if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
                    ESP_LOGW(TAG, "SNTP request failed: %s", esp_err_to_name(ret));
                }
                render_time_page();
            }

            /* Quota is expensive to fetch, so keep it on the slow cadence and
             * let the agent status poll run on every (fast) loop pass. */
            uint32_t now = now_ms();
            bool codex_due = !codex_fetched_once ||
                             (now - last_codex_fetch_ms) >= CODEX_POLL_INTERVAL_MS;
            if (codex_due) {
                codex_fetched_once = true;
                last_codex_fetch_ms = now;
                if (codex_configured()) {
                    render_codex_status("Codex updating");
                    codex_usage_t usage;
                    ret = fetch_codex_usage(&usage);
                    if (ret == ESP_OK) {
                        s_last_codex_usage = usage;
                        render_codex_usage(&s_last_codex_usage, "Online");
                    } else {
                        render_codex_status("Bridge offline");
                    }
                } else {
                    render_codex_status("Edit CODEX URL");
                }
            }

            if (agent_status_configured()) {
                agent_status_t agent;
                if (fetch_agent_status(&agent) == ESP_OK) {
                    s_agent_status = agent;
                    render_agent_status(&s_agent_status, NULL);
                } else {
                    s_agent_status.valid = false;
                    render_agent_detail("Bridge offline");
                }
            } else {
                s_agent_status.valid = false;
                render_agent_detail("Edit AGENT URL");
            }

            /* Sleep until the next poll or a manual refresh (long press). */
            (void)xEventGroupWaitBits(s_wifi_event_group, CODEX_REFRESH_REQUEST_BIT,
                                      pdTRUE, pdFALSE,
                                      pdMS_TO_TICKS(AGENT_POLL_INTERVAL_MS));
        } else if ((bits & WIFI_FAIL_BIT) != 0) {
            render_codex_status("Wi-Fi failed");
            render_time_page();
            EventBits_t recover_bits =
                xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                                    pdMS_TO_TICKS(WIFI_RETRY_BACKOFF_MS));
            if ((recover_bits & WIFI_CONNECTED_BIT) == 0 && s_ap_client_count == 0) {
                reconnect_station();
            }
        } else {
            render_codex_status("Wi-Fi timeout");
            render_time_page();
        }
    }
}

static void weather_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Weather task started");
    esp_err_t ret = start_wifi_station();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi start failed for weather: %s", esp_err_to_name(ret));
        render_weather_status("Wi-Fi start failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        if (!wifi_configured() || s_weather_city[0] == '\0') {
            ESP_LOGW(TAG, "Weather waiting: Wi-Fi or city missing");
            render_weather_status("Open setup AP");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!weather_api_key_configured()) {
            ESP_LOGW(TAG, "Weather waiting: AMap key missing");
            render_weather_status("Set AMap key");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (s_wifi_event_group == NULL) {
            ESP_LOGW(TAG, "Weather waiting: Wi-Fi event group missing");
            render_weather_status("Wi-Fi not started");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if ((bits & WIFI_CONNECTED_BIT) != 0) {
            xEventGroupClearBits(s_wifi_event_group, WEATHER_REFRESH_REQUEST_BIT);
            render_weather_status("Weather updating");

            weather_data_t weather;
            ret = fetch_weather(&weather);
            if (ret == ESP_OK) {
                s_last_weather = weather;
                render_weather(&s_last_weather, "");
            } else {
                ESP_LOGW(TAG, "Weather refresh failed: %s", esp_err_to_name(ret));
                render_weather_status("Weather offline");
            }

            /* Retry soon after a failure; do not clear WIFI_FAIL_BIT here —
             * the codex task owns reconnect handling. */
            uint32_t wait_ms = ret == ESP_OK ? WEATHER_POLL_INTERVAL_MS
                                             : WEATHER_RETRY_INTERVAL_MS;
            (void)xEventGroupWaitBits(s_wifi_event_group,
                                      WEATHER_REFRESH_REQUEST_BIT | WIFI_FAIL_BIT,
                                      pdFALSE, pdFALSE, pdMS_TO_TICKS(wait_ms));
        } else if ((bits & WIFI_FAIL_BIT) != 0) {
            /* Reconnect attempts are owned by codex_usage_task; driving them
             * from here too would defeat the retry backoff. */
            render_weather_status("Wi-Fi failed");
            (void)xEventGroupWaitBits(s_wifi_event_group,
                                      WIFI_CONNECTED_BIT | WEATHER_REFRESH_REQUEST_BIT,
                                      pdFALSE, pdFALSE,
                                      pdMS_TO_TICKS(WIFI_RETRY_BACKOFF_MS));
        } else {
            render_weather_status("Wi-Fi connecting");
            (void)xEventGroupWaitBits(s_wifi_event_group,
                                      WIFI_CONNECTED_BIT | WIFI_FAIL_BIT |
                                          WEATHER_REFRESH_REQUEST_BIT,
                                      pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
        }
    }
}

static void app_display_rounder_event_cb(lv_event_t *event)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(event);
    /* CO5300 requires even start and odd end coordinates. */
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

/*
 * Paint the entire CO5300 GRAM black in the panel's *current* MADCTL state.
 * LVGL only ever writes its 466x466 window (offset by the gap), so the few
 * GRAM columns/rows outside that window keep their random power-on value,
 * which reads as green on this AMOLED. This must run with the SAME rotation
 * that is active at runtime: CO5300 remaps the address window when MV/MX are
 * set, so a pre-rotation clear misses exactly the band that becomes the
 * visible bottom edge afterwards.
 */
static esp_err_t clear_full_panel_gram(esp_lcd_panel_handle_t panel)
{
    const size_t row_bytes = CO5300_GRAM_RES * (BSP_LCD_BITS_PER_PIXEL / 8);
    const size_t buffer_bytes = row_bytes * PANEL_CLEAR_ROWS;
    void *black = heap_caps_calloc(1, buffer_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (black == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_lcd_panel_set_gap(panel, 0, 0);
    for (int y = 0; result == ESP_OK && y < CO5300_GRAM_RES; y += PANEL_CLEAR_ROWS) {
        int rows = CO5300_GRAM_RES - y;
        if (rows > PANEL_CLEAR_ROWS) {
            rows = PANEL_CLEAR_ROWS;
        }
        result = esp_lcd_panel_draw_bitmap(panel, 0, y, CO5300_GRAM_RES, y + rows, black);
    }

    /* QSPI color transfers are queued. Keep the DMA buffer alive until the
     * final batch has drained before returning it to the internal heap. */
    vTaskDelay(pdMS_TO_TICKS(120));
    free(black);
    return result;
}

/*
 * bsp_display_start() hardcodes PSRAM draw buffers. The ESP32-S3 SPI driver
 * cannot DMA from PSRAM, so every flush would malloc an internal bounce
 * buffer of the flush size; once Wi-Fi claims internal heap those
 * allocations fail (spicommon_dma_setup_priv_buffer -> ESP_ERR_NO_MEM) and
 * the panel silently stops receiving updates. Register the display here with
 * internal DMA-capable draw buffers that are allocated once at boot instead.
 */
static lv_display_t *app_display_start(void)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    /* Wi-Fi runs at priority 23 on core 0; keep the LVGL worker on core 1 so
     * scans cannot starve it while it holds the LVGL lock. */
    adapter_cfg.task_core_id = 1;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter init failed");
        return NULL;
    }

    const bsp_display_config_t panel_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * APP_DRAW_BUFFER_HEIGHT * (BSP_LCD_BITS_PER_PIXEL / 8),
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    if (bsp_display_new(&panel_cfg, &panel, &panel_io) != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel init failed");
        return NULL;
    }
    /* Flood the whole GRAM black before rotating so any cell LVGL's offset
     * window never touches is black (invisible on the watchface) instead of
     * its random green power-on value. */
    if (clear_full_panel_gram(panel) != ESP_OK) {
        ESP_LOGW(TAG, "Pre-rotation CO5300 GRAM clear failed");
    }
    if (bsp_display_rotation_set(APP_DISPLAY_ROTATION) != ESP_OK) {
        ESP_LOGW(TAG, "Panel rotation set failed");
    }
    /* MADCTL swaps the module's 6-px calibrated X offset onto Y and mirrors
     * it. y=8 is the empirical sweet spot: 6 and 8 both leave only ~5 edge
     * pixels at the very bottom centre, while 0 and 10 make them taller. The
     * residual sits at a GRAM address the black clear cannot reach and the
     * 2-px draw alignment cannot cover - a panel-edge quantization artifact. */
    (void)esp_lcd_panel_set_gap(panel, 0, 8);

    const esp_lv_adapter_display_config_t display_cfg = {
        .panel = panel,
        .panel_io = panel_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = APP_DRAW_BUFFER_HEIGHT,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
            .mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&display_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "LVGL display register failed");
        return NULL;
    }
    lv_display_add_event_cb(disp, app_display_rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    const bsp_display_cfg_t touch_bsp_cfg = {
        .touch_flags = {
            .swap_xy = APP_TOUCH_SWAP_XY,
            .mirror_x = APP_TOUCH_MIRROR_X,
            .mirror_y = APP_TOUCH_MIRROR_Y,
        },
    };
    esp_lcd_touch_handle_t touch = NULL;
    if (bsp_touch_new(&touch_bsp_cfg, &touch) != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed");
        return NULL;
    }
    const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch);
    if (esp_lv_adapter_register_touch(&touch_cfg) == NULL) {
        ESP_LOGE(TAG, "Touch register failed");
        return NULL;
    }

    if (bsp_display_brightness_init() != ESP_OK) {
        ESP_LOGE(TAG, "Brightness init failed");
        return NULL;
    }
    if (esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter start failed");
        return NULL;
    }
    return disp;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_wifi_credentials();
    load_ui_settings();
    set_default_clock();
    s_last_activity_ms = now_ms();

    if (app_display_start() == NULL) {
        ESP_LOGE(TAG, "Display start failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    (void)bsp_display_brightness_set(s_cfg_brightness);

    if (bsp_display_lock(DISPLAY_LOCK_TIMEOUT_MS) == ESP_OK) {
        create_app_ui();
        bsp_display_unlock();
    }
    render_time_page();
    render_weather_status(wifi_configured() ? "Wi-Fi not started" : "Edit Wi-Fi config");
    render_pedometer(0, 0, "Waiting for QMI8658");
    render_codex_status(wifi_configured() ? "Wi-Fi not started" : "Edit Wi-Fi config");
    request_full_screen_refresh();

    init_reset_button();
    init_battery_monitor();
    ret = start_wifi_station();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi start failed at boot: %s", esp_err_to_name(ret));
        render_weather_status("Wi-Fi start failed");
        render_codex_status("Wi-Fi start failed");
    }
    xTaskCreate(clock_task, "clock_task", 4096, NULL, 3, NULL);

    qmi8658_dev_t *dev = NULL;
    ret = init_qmi8658(&dev);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Pedometer ready");
        render_pedometer(0, 0, "Tracking steps");
        xTaskCreate(pedometer_task, "pedometer_task", 4096, dev, 5, NULL);
    } else {
        render_pedometer(0, 0, "QMI8658 offline");
    }

    if (xTaskCreate(weather_task, "weather_task", 5120, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Weather task create failed");
        render_weather_status("Weather task failed");
    } else {
        ESP_LOGI(TAG, "Weather task created");
    }
    if (xTaskCreate(codex_usage_task, "codex_usage_task", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Codex task create failed");
        render_codex_status("Codex task failed");
    }
    if (xTaskCreate(music_audio_task, "music_audio", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Music audio task create failed");
    }
    xTaskCreate(screenshot_console_task, "shot_console", SCREENSHOT_CMD_TASK_STACK, NULL, 2,
                NULL);
}

#pragma once

/*
 * Defaults are placeholders safe to publish. Put your real values in
 * app_config_local.h next to this file (gitignored) - it overrides any
 * of the defines below. See app_config_local.h.example.
 */
#if __has_include("app_config_local.h")
#include "app_config_local.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "ntp.aliyun.com"
#endif
/* Served by tools/codex_usage_server.py on your PC. */
#ifndef CODEX_USAGE_URL
#define CODEX_USAGE_URL "http://YOUR_PC_LAN_IP:8765/usage"
#endif
/* Served by tools/agent_status_server.py; fed by Claude Code / Codex hooks. */
#ifndef AGENT_STATUS_URL
#define AGENT_STATUS_URL "http://YOUR_PC_LAN_IP:8766/status"
#endif
#ifndef WEATHER_CITY
#define WEATHER_CITY "青岛市"
#endif
#ifndef AMAP_WEATHER_KEY
#define AMAP_WEATHER_KEY "YOUR_AMAP_WEB_SERVICE_KEY"
#endif

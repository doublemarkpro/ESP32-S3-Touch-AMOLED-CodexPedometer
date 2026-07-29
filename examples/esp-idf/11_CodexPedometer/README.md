# Codex Pedometer

A multi-page smartwatch UI for the ESP32-S3-Touch-AMOLED-1.75: analog watch faces,
weather, step counting, Codex quota, AI agent status, a microphone spectrum and a
stock ticker — switchable between Chinese and English.

![Clock, weather, Codex and AI status pages](docs/overview.png)

![Music, stock and settings pages](docs/overview2.png)

> The images are renderings produced by `tools/render_docs_shots.py` from the same
> geometry and colours the firmware uses, not photographs of the panel.

## Pages

Swipe left/right to change page; swipe down from the top for settings.

| Page | What it shows |
| --- | --- |
| Clock | Analog hands plus a digital readout. Three faces — INFO, PIXEL, TILES — and four tappable corner complications that cycle through battery, temperature, weekday, date, weather, Codex quota and AI state. |
| Weather | Current conditions from AMap, today's low/high, and a 4-day forecast detail page behind the weather icon. |
| Steps | QMI8658 step count against a configurable goal, with distance, calories and activity. |
| Codex | Remaining quota as the headline, plus Codex's own working state: the icon takes the state colour, one column carries the state word, and a line names the project it is running. |
| AI status | Codex and Claude Code state from the PC bridge — idle, working, waiting for approval, error, done — with a breathing ring that replaces a desk status lamp. |
| Music | Microphone spectrum with a VU ring, and bass / volume / treble readouts. |
| Stock | Live quotes for a configurable watchlist, an intraday trace, and 20-day candles behind a tap. Red for up, green for down. |

## Hardware

- ESP32-S3-Touch-AMOLED-1.75 (466x466 round CO5300 QSPI AMOLED)
- CST9217 capacitive touch
- QMI8658 IMU (steps)
- ES7210 microphone (spectrum)
- AXP2101 PMU (battery)

## Supported versions

- ESP-IDF `v6.0.2`
- Target `esp32s3`

## Build and flash

From the repository root with ESP-IDF activated:

```bash
idf.py -C examples/esp-idf/11_CodexPedometer set-target esp32s3 build
```

```bash
idf.py -C examples/esp-idf/11_CodexPedometer -p COM4 flash monitor
```

The app partition is 8 MB (`partitions.csv`) because the bundled GB2312 fonts are
large; the board has 16 MB, so this only claims unused space. A 512 KB `wallpaper`
partition follows the app.

## First-time setup

1. On first boot the device brings up a `CodexPedometer` softAP.
2. Join it and open `http://192.168.4.1/`.
3. The page configures Wi-Fi, the weather city and AMap key, the step goal, the
   stock watchlist (`sz002241:歌尔股份,hk09903:天数智芯`), the ring colours, the
   watch face and the interface language.
4. Once Wi-Fi is up the same page is reachable at the device's LAN address, which
   the settings panel shows.

Credentials can also be compiled in: copy the placeholders from `main/app_config.h`
into `main/app_config_local.h` (gitignored) and fill them in.

## PC bridge

`tools/agent_status_server.py` feeds the Codex, AI status and stock pages from a
machine on the same LAN:

| Endpoint | Serves |
| --- | --- |
| `:8765/usage` | Codex quota |
| `:8766/status` | Codex / Claude Code agent state |
| `:8766/stock?code=` | Intraday and daily series — the public chart endpoints redirect to HTTPS, which this build cannot reach |
| `:8766/weather?city=` | Current conditions and today's range, from Open-Meteo (no key) |
| `:8766/quote?codes=` | Live quotes, names included — the feed speaks GBK, which the watch cannot decode |
| `:8766/claude` | Tokens and turns Claude Code has spent in a rolling 5-hour window |

```bash
python tools/agent_status_server.py --install-autostart
```

Agent state is pushed by hooks: Claude Code posts to the bridge natively, Codex
through `tools/codex_hook.py` registered in `~/.codex/hooks.json`.

## Desktop widget

The same data, as a frameless always-on-top panel for the desktop - useful when
the watch is not in front of you:

```bash
python tools/agent_widget.py
```

![Desktop widget](docs/widget.png)

Three tabs, one at a time — the same numbers the watch shows, in the same
colours:

| Tab | Shows |
| --- | --- |
| AI | Agent state and target, Codex quota remaining, Claude's token spend |
| 天气 | Today's temperature, condition and range, then the next three days |
| 股票 | Price and change, an intraday trace or 20-day candles, and your own symbol list |

The lamp in the header stays visible on every tab, so switching away from AI
does not cost you the one thing worth glancing at. Only the open tab is polled.

On the stock tab: click a symbol chip to switch, click 日内 / 日K to change the
chart, and type codes into the box (`sz002241,sh600519,hk09903`) then 应用 to
change the list. Tab, symbols and chart mode are remembered.

```bash
python tools/agent_widget.py --city 上海 --codes sz002241,sh600519
```

Drag it anywhere (the position is remembered), right-click for opacity and
always-on-top, `--host` if the bridge runs on another machine, and
`--install-autostart` to start it at logon. Standard library only.

**On Claude's usage:** there is no local record of the plan's rate-limit
percentage — that lives server-side behind the account's credentials — so the
widget reports what can honestly be measured on this machine: tokens and turns
summed from Claude Code's own transcripts over a rolling 5-hour window. Cache
reads are counted separately and left out of the headline; they dwarf
everything else and say nothing about how hard the model is working.

## Assets

Regenerate rather than hand-edit:

```bash
python tools/build_cjk_font.py --size 16
```

```bash
python tools/build_ui_icons_extra.py --preview
```

```bash
python tools/make_wallpapers.py
```

```bash
python tools/set_wallpaper.py wallpapers/aurora_nebula.png
```

```bash
python tools/render_docs_shots.py
```

## Runtime notes

- **Draw buffers must be internal RAM.** The ESP32-S3 cannot DMA out of PSRAM, so
  the SPI driver allocates a bounce buffer per transfer and silently stops
  flushing once Wi-Fi has claimed the internal heap. `app_display_start()`
  registers the panel with internal buffers for this reason.
- **LVGL's heap lives in PSRAM** via `LV_MEM_POOL_ALLOC`, passed as raw compile
  options in the project CMakeLists — CMake drops function-style definitions.
- **`LV_FONT_FMT_TXT_LARGE` is on.** A full GB2312 cut at 22 px passes 1 MB of
  bitmap data, which overflows LVGL's 20-bit `bitmap_index`.
- **Canvas buffers are not owned by LVGL.** `lv_canvas_set_buffer()` does not take
  ownership and `lv_obj_clean()` will not free it, so every canvas here carries an
  `LV_EVENT_DELETE` handler; without it each watch-face rebuild leaked 434 KB.
- Roughly five pixels at the bottom centre stay lit: after the 90° rotation the
  CO5300's GRAM window quantises to the nearest column pair, and no gap value
  clears the remainder.

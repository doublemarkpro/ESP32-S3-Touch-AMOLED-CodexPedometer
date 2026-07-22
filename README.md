<div align="center">
  <h1>ESP32-S3-Touch-AMOLED-1.75</h1>
  <p><strong>ESP32-S3 1.75-inch 466 x 466 QSPI AMOLED touch development board</strong></p>
  <p>
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/actions/workflows/examples.yml"><img alt="Build Examples" src="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/actions/workflows/examples.yml/badge.svg"></a>
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/releases/latest"><img alt="Latest Release" src="https://img.shields.io/github/v/release/waveshareteam/ESP32-S3-Touch-AMOLED-1.75"></a>
    <a href="LICENSE"><img alt="License" src="https://img.shields.io/github/license/waveshareteam/ESP32-S3-Touch-AMOLED-1.75"></a>
  </p>
  <p>
    <a href="https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm">🌐 Product Page</a> ·
    <a href="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/releases/latest">📦 Firmware Releases</a> ·
    <a href="examples/esp-idf/">🧩 ESP-IDF Examples</a> ·
    <a href="examples/arduino/">🔧 Arduino Examples</a> ·
    <a href="docs/">📚 Documentation</a>
  </p>
</div>

---

## ✨ Overview

This repository provides example software, source-built firmware packages,
factory recovery firmware, schematics, and documentation for the Waveshare
ESP32-S3-Touch-AMOLED-1.75.

The board combines an ESP32-S3 with a high-resolution AMOLED display,
capacitive touch, motion sensing, power management, real-time clock, audio,
and storage interfaces in a compact development platform.

## CodexPedometer Smart Watch Demo

This fork adds a full ESP-IDF watch-style demo at
[`examples/esp-idf/11_CodexPedometer`](examples/esp-idf/11_CodexPedometer/).

It turns the board into a compact round watch UI with:

- analog clock page with NTP time sync and Chinese date labels
- weather page using AMap Web Service weather data
- QMI8658 pedometer page with distance, kcal, and movement counters
- Codex quota page that polls a local PC bridge
- AI agent status page that shows Codex / Claude activity as a Wi-Fi status lamp
- phone-style setup portal at `http://192.168.4.1`
- Windows helper bridge for Codex usage and agent lifecycle status
- optional 3D-printable battery shell draft under
  [`mechanical/esp32_s3_touch_amoled_case`](mechanical/esp32_s3_touch_amoled_case/)

Recent work includes the AI status page, smooth status breathing/blinking,
diagnostic hook logging, duplicate bridge-instance protection, hidden startup
launch, Codex project-name reporting, and cleaner waiting/error/done states.

### Quick Start

Use ESP-IDF v6.0.2 or newer. Open an ESP-IDF terminal and build the demo:

```powershell
cd examples\esp-idf\11_CodexPedometer
idf.py set-target esp32s3
idf.py build
idf.py -p COM4 flash monitor
```

Replace `COM4` with your board port. In VS Code, open the
`examples/esp-idf/11_CodexPedometer` folder directly, select target `esp32s3`,
then use ESP-IDF Build / Flash.

### Device Setup Portal

On first boot the firmware starts an open AP named `CodexPedometer`.
Connect a phone or PC to that AP and open:

```text
http://192.168.4.1
```

The setup page lets you save:

- Wi-Fi SSID and password
- weather city, for example `青岛市`, `上海市`, or an AMap adcode such as `370200`
- AMap Web Service Key
- Codex usage bridge URL
- AI agent status bridge URL
- pedometer step target

The values are stored in NVS, so normal users do not need to edit firmware
source before flashing. The default placeholders live in
[`app_config.h`](examples/esp-idf/11_CodexPedometer/main/app_config.h).

### Codex Usage Bridge

The Codex quota page polls a small local HTTP bridge running on your PC:

```powershell
python tools\codex_usage_server.py
```

It listens on:

```text
http://<your-pc-lan-ip>:8765/usage
```

The bridge tries to read real Codex quota data through Codex Desktop's
`codex app-server --stdio` account APIs. If that is unavailable, it falls back
to [`tools/codex_usage.json`](tools/codex_usage.json), so the watch UI still has
predictable data during development.

Point the device setup portal's Codex URL at:

```text
http://<your-pc-lan-ip>:8765/usage
```

### AI Agent Status Bridge

The AI status page polls:

```text
http://<your-pc-lan-ip>:8766/status
```

Start the bridge manually:

```powershell
python tools\agent_status_server.py
```

Install it to start automatically at Windows logon:

```powershell
python tools\agent_status_server.py --install
```

Remove the autostart entry:

```powershell
python tools\agent_status_server.py --uninstall
```

The installer first tries to create a Windows scheduled task named
`AgentStatusBridge`. If Windows refuses that, it falls back to a hidden startup
folder launcher. Runtime output goes to `tools/agent_status.log`, which is
ignored by Git. A second bridge instance refuses to bind the port, so duplicate
startup entries fail loudly instead of splitting hook events between two
processes.

Print hook examples:

```powershell
python tools\agent_status_server.py --print-hooks
```

For Codex hooks, call the shim:

```text
py -3 tools\codex_hook.py --state working
py -3 tools\codex_hook.py --state done
```

For Claude Code, merge the printed hook JSON into `~/.claude/settings.json`.
Supported states are `idle`, `working`, `waiting`, `error`, and `done`.

### Open Source Configuration Notes

Do not commit personal Wi-Fi passwords, AMap keys, Codex bridge IPs, runtime
logs, or captured screenshots. Use the setup portal or local-only edits for
private values. The committed defaults are placeholders by design.

## 🖥️ Hardware Overview

| Feature | Device / interface |
| --- | --- |
| MCU | ESP32-S3 with 16 MB flash |
| Display | 1.75-inch 466 x 466 QSPI AMOLED |
| Touch | CST9217 capacitive touch controller with two-point support |
| Power management | AXP2101 |
| Motion sensor | QMI8658 six-axis IMU |
| Real-time clock | PCF85063 |
| Audio | Dual onboard digital microphones and ES8311 codec support |
| Storage | microSD card interface |
| Expansion example | External LC76G GNSS module over I2C |
| Board support | Managed component: `waveshare/esp32_s3_touch_amoled_1_75` |
| Hardware files | [Product wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) and [schematics](Schematic/) |

## 📦 Firmware Releases

The fastest way to try an example is to use a ready-to-flash package from the
[latest release](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/releases/latest).

1. Download the `*-combined.zip` package for the example and framework version
   you need.
2. Extract the archive and install esptool with
   `python -m pip install esptool`.
3. Connect the board over USB.
4. Run `flash_combined.bat COMx` on Windows or
   `./flash_combined.sh /dev/ttyACM0` on Linux.
5. Reset the board if it does not restart automatically.

> [!NOTE]
> Combined images are flashed at offset `0x0`. Each package also contains the
> original split binaries, flash arguments, helper scripts, and checksums.

Factory recovery images under [firmware](firmware/) are separate from
CI-generated example firmware. See
[Firmware and Factory Recovery](docs/firmware.md) for details.

## 🧪 Examples

### ESP-IDF

| Example | Focus |
| --- | --- |
| [01_AXP2101](examples/esp-idf/01_AXP2101/) | Power management, charging, and battery telemetry |
| [02_lvgl_demo_v9](examples/esp-idf/02_lvgl_demo_v9/) | LVGL 9 display benchmark |
| [03_esp-brookesia](examples/esp-idf/03_esp-brookesia/) | ESP-Brookesia phone-style application UI |
| [04_Immersive_block](examples/esp-idf/04_Immersive_block/) | Motion-controlled falling-block demo |
| [05_Spec_Analyzer](examples/esp-idf/05_Spec_Analyzer/) | Microphone FFT spectrum analyzer |
| [11_CodexPedometer](examples/esp-idf/11_CodexPedometer/) | Watch UI, pedometer, weather, Codex quota, AI status bridge |

### Arduino

| Example | Focus |
| --- | --- |
| [01_HelloWorld](examples/arduino/01_HelloWorld/) | Display bring-up and text output |
| [02_GFX_AsciiTable](examples/arduino/02_GFX_AsciiTable/) | GFX text and character rendering |
| [03_LVGL_PCF85063_simpleTime](examples/arduino/03_LVGL_PCF85063_simpleTime/) | LVGL real-time clock interface |
| [04_LVGL_QMI8658_ui](examples/arduino/04_LVGL_QMI8658_ui/) | LVGL accelerometer and gyroscope interface |
| [05_LVGL_AXP2101_ADC_Data](examples/arduino/05_LVGL_AXP2101_ADC_Data/) | LVGL power and battery telemetry |
| [06_LVGL_Widgets](examples/arduino/06_LVGL_Widgets/) | LVGL music UI, touch input, and IMU integration |
| [07_LVGL_SD_Test](examples/arduino/07_LVGL_SD_Test/) | microSD access through an LVGL application |
| [08_ES8311](examples/arduino/08_ES8311/) | ES8311 audio path and LVGL interface |
| [09_LC76G_I2C](examples/arduino/09_LC76G_I2C/) | External LC76G GNSS over I2C |
| [10_Touch_CST9217](examples/arduino/10_Touch_CST9217/) | Raw interrupt-driven single- and two-point touch diagnostics |

Bundled Arduino libraries live under
[`examples/arduino/libraries`](examples/arduino/libraries/). Their upstream
library examples are intentionally excluded from the product CI matrix.

## 🛠️ Supported Toolchains

| Surface | Version | Firmware builds |
| --- | --- | ---: |
| ESP-IDF | `v5.5.4` | 5 |
| ESP-IDF | `v6.0.2` | 5 |
| Arduino-ESP32 | `3.3.10` | 10 |

The [Build Examples workflow](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/actions/workflows/examples.yml)
runs two discovery jobs and 20 firmware build jobs. Each successful build is
packaged as a flashable firmware artifact. Bundled library examples and factory
recovery binaries are intentionally excluded. See
[Continuous Integration](docs/ci.md) for matrix and dispatch details.

The historical v1.0.1 release predates `10_Touch_CST9217` and contains 19
firmware packages.

## 🗂️ Repository Layout

| Path | Purpose |
| --- | --- |
| [`examples/esp-idf/`](examples/esp-idf/) | First-party ESP-IDF projects |
| [`examples/arduino/`](examples/arduino/) | First-party Arduino sketches and bundled libraries |
| [`firmware/`](firmware/) | Factory flashing and recovery binary |
| [`releases/`](releases/) | Packaging, artifact download, and release tools |
| [`config/`](config/) | Shared ESP-IDF compatibility overlays |
| [`docs/`](docs/) | Setup, example, CI, component, firmware, and troubleshooting notes |
| [`Schematic/`](Schematic/) | Public schematic files |
| [`scripts/`](scripts/) | CI example discovery helpers |
| [`.github/`](.github/) | GitHub Actions and contribution templates |

## 📚 Documentation

- [Getting Started](docs/getting-started.md)
- [Example Catalog](docs/examples.md)
- [Repository Structure](docs/repository-structure.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Firmware and Factory Recovery](docs/firmware.md)
- [Continuous Integration](docs/ci.md)
- [Components](docs/components.md)
- [Release Tools](releases/README.md)
- [Changelog](CHANGELOG.md)

## 🤝 Support and Contributions

Contributions and reproducible issue reports are welcome. Include the example
path, framework version, reproduction steps, expected behavior, actual
behavior, and relevant serial logs.

- [Contributing Guide](CONTRIBUTING.md)
- [Support](SUPPORT.md)
- [Security Policy](SECURITY.md)
- [Open an Issue](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/issues/new/choose)

## 📄 License

This repository is licensed under the Apache License 2.0. See
[LICENSE](LICENSE).

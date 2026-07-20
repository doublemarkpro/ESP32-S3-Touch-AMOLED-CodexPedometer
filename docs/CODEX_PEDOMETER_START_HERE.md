# ESP32-S3-Touch-AMOLED-1.75: from zero to Codex pedometer

This project directory is intended to live at:

```text
D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75
```

## What is already set up

- Official Waveshare repository root files were cloned into the project directory.
- Arduino CLI 1.5.1 was installed locally at `D:\ESP32Projects\tools\arduino-cli`.
- Arduino CLI config lives at `D:\ESP32Projects\tools\arduino-cli.yaml`.
- Arduino data/cache/sketchbook directories are pinned under `D:\ESP32Projects`.
- The ESP32 package index was downloaded.
- The custom `11_CodexPedometer` sketch, helper scripts, and Codex usage bridge were added.
- `esp32:esp32@3.3.10` installation was attempted, but the GitHub toolchain download failed twice on `xtensa-esp-elf`.
- The official Arduino library tree is not fully fetched yet because GitHub downloads timed out during sparse checkout.

Retry these when the network is steadier:

```powershell
D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75\scripts\fetch_arduino_sources.ps1
D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75\scripts\install_esp32_core.ps1
```

## Board facts used by this sketch

- Display: 1.75 inch 466 x 466 CO5300 AMOLED over QSPI.
- Touch: CST9217 over I2C.
- IMU: QMI8658 six-axis sensor; its hardware pedometer is used.
- Power management: AXP2101.
- The board has 16 MB flash and 8 MB PSRAM.

## First bring-up path

1. Plug the board in with a data-capable USB-C cable.
2. Run:

```powershell
D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75\scripts\list_boards.ps1
```

3. Note the COM port, for example `COM5`.
4. Open:

```text
D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75\examples\arduino\11_CodexPedometer\11_CodexPedometer.ino
```

5. Replace `YOUR_WIFI_SSID`, `YOUR_WIFI_PASSWORD`, and `CODEX_USAGE_URL`.
6. Compile:

```powershell
D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75\scripts\compile_codex_pedometer.ps1
```

7. Upload:

```powershell
D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75\scripts\upload_codex_pedometer.ps1 -Port COM5
```

If upload cannot enter download mode, hold BOOT, tap RESET, start upload, then release BOOT after transfer begins.

## Codex usage display

The board polls a tiny local HTTP service:

```powershell
D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75\scripts\serve_codex_usage.ps1
```

Edit:

```text
D:\ESP32Projects\ESP32-S3-Touch-AMOLED-1.75\tools\codex_usage.json
```

The ESP32 displays `label`, `used_tokens`, `limit_tokens`, and `updated`.

Codex Desktop does not currently expose a simple public LAN endpoint for usage data, so this service is a bridge you can later connect to a real usage source. For now it proves the full screen + Wi-Fi + JSON polling path.

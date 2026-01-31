# NukaMiner (PlatformIO) - LILYGO T-Dongle-S3 Duino-Coin Miner

This is a **PlatformIO / VS Code** project for the **LILYGO T-Dongle-S3** that:
- Mines **Duino-Coin** using the **official DuinoMinerESP32 MiningJob/DSHA1 code** (adapted)
- Uses a built-in **WiFi captive portal** for first-time configuration (AP password: `nukaminer`)
- Shows a simple 3-page UI (Logo / Mining / Setup) on the LCD
- Supports **display sleep** after N seconds (0 = never); wake via the BOOT button
- Can **backup/restore config to SD card** if you inserted one (file: `/nukaminer.json`)

## Controls
- **Short press BOOT**: cycle pages
- **Long press BOOT**: start config portal (AP)

## Captive portal
When running, connect to the AP (e.g. `NukaMiner-<id>`) then open any URL, or go to `http://192.168.4.1/`.

## SD backup/restore
If an SD card is present and inserted:
- `http://192.168.4.1/backup`  -> writes `/nukaminer.json`
- `http://192.168.4.1/restore` -> reads `/nukaminer.json` and saves to NVS

> Note: T-Dongle-S3 does include a built-in TF slot. This feature is only active if you have an SD Card inserted.

## Pin notes
- Display config is provided via `include/User_Setup.h` (TFT_eSPI).
- Backlight pins: `38` (primary) and `37` (also pulled low for compatibility).
- SD CS default: `10` (edit in `src/main.cpp` if needed).

## Build/Upload
In VS Code with PlatformIO:
1. Open this folder
2. Select environment **t-dongle-s3**
3. Build / Upload
4. Open Serial Monitor at **115200**

## Web UI

When connected to your WiFi, open the device IP in a browser (default port 80).

- Default credentials: **admin / nukaminer**
- Dashboard: `/`
- Settings: `/settings`
- Live status JSON: `/status.json`

If you disable the Web UI, you can still re-enable it by holding **BOOT** to start the captive portal.

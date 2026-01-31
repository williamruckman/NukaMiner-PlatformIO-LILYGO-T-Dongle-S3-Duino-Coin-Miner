// Setup for LILYGO T-Dongle-S3 (ESP32-S3 + ST7735 0.96" 80x160 IPS)
//
// This matches the known-working TzCoinMiner setup:
// - Uses ST7735_GREENTAB160x80 init profile
// - Uses SPI_FREQUENCY 27MHz (more stable on this wiring)
#ifndef USER_SETUP_H
#define USER_SETUP_H

#define USER_SETUP_ID 7735
#define USER_SETUP_INFO "T-Dongle-S3 ST7735 80x160"

// --- Driver ---
#define ST7735_DRIVER

// Use the dedicated 160x80 init profile (sets correct RAM offsets + inversion/BGR)
#define ST7735_GREENTAB160x80

// --- Display size (native portrait) ---
#define TFT_WIDTH  80
#define TFT_HEIGHT 160

// --- SPI pins ---
#define TFT_MISO -1
#define TFT_MOSI 3
#define TFT_SCLK 5
#define TFT_CS   4
#define TFT_DC   2
#define TFT_RST  1

// --- Backlight ---
#define TFT_BL 37
// T-Dongle-S3 backlight is ACTIVE-LOW on many units.
#define TFT_BACKLIGHT_ON LOW

// --- Fonts ---
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// Force HSPI port on ESP32-S3 (workaround for some TFT_eSPI init panics)
#define USE_HSPI_PORT
// --- SPI settings ---
#define SPI_FREQUENCY  27000000
#define SPI_READ_FREQUENCY  16000000
#define SPI_TOUCH_FREQUENCY  2500000

#endif // USER_SETUP_H

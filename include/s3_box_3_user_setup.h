// SquachWatch-CYD — TFT_eSPI setup for the Espressif ESP32-S3-Box-3 2.4"
//
// Pulled in via -include in platformio.ini's [env:s3-box-3], ahead of
// TFT_eSPI's own User_Setup, so nothing in the library tree is edited.
//
// Hardware: ESP32-S3-WROOM-1-N16R8 (16 MB quad flash, 8 MB octal PSRAM),
// native USB on GPIO 19/20, and a 320x240 ILI9342C display on SPI.
// Pins from Espressif's esp-box-3 BSP: MOSI 6, SCLK 7, CS 5, DC 4, MISO not
// wired (-1), and backlight 47 (HIGH is on).
//
// Hardware quirk: LCD reset on GPIO 48 is wired through an inverting
// transistor (Active-High reset: HIGH holds the panel in reset, LOW runs).
// TFT_RST must remain -1 here so TFT_eSPI does not leave GPIO 48 pulled HIGH;
// instead, setup() manually pulses GPIO 48 HIGH -> LOW before tft.init().
//
// Touch is not on this bus: a GT911 on I2C (SDA 8, SCL 18, INT 3, 0x5D/0x14),
// shared with the ES8311 audio codec at 0x18 (I2S on MCLK 2, BCLK 17, WS 45,
// DOUT 15; NS4150 amp enable on GPIO 46 HIGH). See setup().
#pragma once

#define USER_SETUP_INFO    "SquachWatch-CYD / ESP32-S3-Box-3 2.4 inch / ILI9341"

#define ILI9341_DRIVER
#define TFT_WIDTH   240
#define TFT_HEIGHT  320

#define USE_HSPI_PORT
#define TFT_MISO  -1   // not wired on S3-Box-3 LCD header
#define TFT_MOSI   6
#define TFT_SCLK   7
#define TFT_CS     5
#define TFT_DC     4
#define TFT_RST   -1   // GPIO 48 is active-high reset! Pulsed HIGH->LOW in setup()
#define TFT_BL    47
#define TFT_BACKLIGHT_ON HIGH

// The same fonts every other CYD build loads, and no more.
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define SMOOTH_FONT

// 27 MHz (rated frequency for the S3-Box-3 panel over GPIO matrix in TFT_eSPI Setup 251)
#ifndef SPI_FREQUENCY
#define SPI_FREQUENCY         27000000
#endif
#define SPI_READ_FREQUENCY    20000000

#define TFT_INVERSION_ON
#define TFT_RGB_ORDER TFT_BGR

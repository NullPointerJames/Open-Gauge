#pragma once

#include <stdint.h>

// ─── LilyGo T-Circle-S3 Pin Definitions ──────────────────────────────────────

// QSPI LCD (SH8601, 466x466 round AMOLED)
#define LCD_CS          10
#define LCD_PCLK        12
#define LCD_DATA0       11
#define LCD_DATA1       13
#define LCD_DATA2       14
#define LCD_DATA3       15
#define LCD_RST         17
#define LCD_EN          16   // Display power enable -- drive HIGH before init

// Capacitive touch (CST816S over I2C)
#define TP_SDA          7
#define TP_SCL          6
#define TP_INT          9
#define TP_RST          -1   // Not connected on T-Circle-S3
#define I2C_HOST_ID     I2C_NUM_0

// ─── Display Parameters ───────────────────────────────────────────────────────

// Raw panel RAM dimensions (SH8601 has a 6-column hardware addressing offset)
#define LCD_H_RES           472
#define LCD_V_RES           466
#define LCD_COL_OFFSET      6
#define LCD_ROW_OFFSET      0
#define LCD_BIT_PER_PIXEL   16

// Logical display dimensions exposed to LVGL (visible circular area)
#define DISP_WIDTH          466   // LCD_H_RES - LCD_COL_OFFSET
#define DISP_HEIGHT         466
#define DISP_CENTER_X       233
#define DISP_CENTER_Y       233
#define DISP_RADIUS         233

// ─── QSPI Bus ─────────────────────────────────────────────────────────────────

#define LCD_SPI_HOST        SPI2_HOST
#define LCD_SPI_CLK_HZ      (40 * 1000 * 1000)  // 40 MHz -- safe limit for QSPI AMOLED

// ─── LVGL Frame Buffer ────────────────────────────────────────────────────────
// Buffers live in PSRAM (8 MB available). Partial rendering at 1/10 of screen.
#define LVGL_BUF_LINES      (DISP_HEIGHT / 10)

// ─── Default CAN/TWAI Pins (user-configurable via web UI) ────────────────────
#define DEFAULT_CAN_TX      43
#define DEFAULT_CAN_RX      44

// ─── Application Version ─────────────────────────────────────────────────────
#define APP_VERSION  "1.0.0"
#define APP_NAME     "OpenGauge"

#pragma once

/* 板级配置: LCDWiki 2.8inch ESP32-S3 Display (ES3C28P)
 * 资料: https://www.lcdwiki.com/zh/2.8inch_ESP32-S3_Display
 * 主控: ESP32-S3 N16R8 (16MB QSPI Flash + 8MB OPI PSRAM)
 */

/* ── LCD: ILI9341V, 4-line SPI, 320x240 横屏 ── */
#define BOARD_LCD_HOST      SPI3_HOST
#define BOARD_LCD_SCLK      12
#define BOARD_LCD_MOSI      11
#define BOARD_LCD_MISO      13
#define BOARD_LCD_CS        10
#define BOARD_LCD_DC        46
#define BOARD_LCD_RST       (-1)              /* 屏复位与 ESP32 EN 共用, 软件不接管 */
#define BOARD_LCD_BL        45
#define BOARD_LCD_H_RES     320
#define BOARD_LCD_V_RES     240
#define BOARD_LCD_SWAP_XY   true              /* 原生 240x320 → 横屏 320x240 */
#define BOARD_LCD_MIRROR_X  false
#define BOARD_LCD_MIRROR_Y  false
#define BOARD_LCD_INVERT    false
#define BOARD_LCD_SPI_HZ    (40 * 1000 * 1000)

/* ── 电容触摸: FT6336G ── */
#define BOARD_TOUCH_I2C_PORT I2C_NUM_0
#define BOARD_TOUCH_SDA      16
#define BOARD_TOUCH_SCL      15
#define BOARD_TOUCH_RST      18
#define BOARD_TOUCH_INT      17
#define BOARD_TOUCH_ADDR     0x38

/* ── 音频: I2S → ES8311 codec → FM8002E 功放 ── */
#define BOARD_I2S_PORT       I2S_NUM_0
#define BOARD_I2S_MCLK       4
#define BOARD_I2S_BCLK       5
#define BOARD_I2S_WS         7
#define BOARD_I2S_DOUT       8
#define BOARD_I2S_DIN        6                 /* 麦克风输入 (本项目未使用) */
#define BOARD_CODEC_I2C_PORT I2C_NUM_0
#define BOARD_CODEC_I2C_SDA  16
#define BOARD_CODEC_I2C_SCL  15
#define BOARD_CODEC_ADDR     0x18              /* ES8311 默认地址 */
#define BOARD_PA_EN_GPIO     1
#define BOARD_PA_EN_LEVEL    0                 /* 低电平使能功放 */

/* ── microSD: SDIO 4-bit ── */
#define BOARD_SD_CLK         38
#define BOARD_SD_CMD         40
#define BOARD_SD_D0          39
#define BOARD_SD_D1          41
#define BOARD_SD_D2          48
#define BOARD_SD_D3          47

/* ── 电池电压 ADC: IO9 = ADC1 CH8 ── */
#define BOARD_VBAT_ADC_UNIT  ADC_UNIT_1
#define BOARD_VBAT_ADC_CHAN  ADC_CHANNEL_8

/* ── 按键 / 指示灯 ── */
#define BOARD_KEY_PWR        0                 /* BOOT 键 (RTC GPIO, 支持 ext0 唤醒) */
#define BOARD_KEY_ACTIVE     0                 /* 按下为低电平 */
#define BOARD_LED_RGB        42                /* WS2812 单线 RGB 灯 */

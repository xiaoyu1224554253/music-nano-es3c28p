#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "board_config.h"
#include "drv_display.h"

static const char *TAG = "drv_display";

static void lcd_backlight_init(void);

/* ────────────────────────── LCD (ILI9341) ────────────────────────── */
#define PIN_LCD_SCLK  BOARD_LCD_SCLK
#define PIN_LCD_MOSI  BOARD_LCD_MOSI
#define PIN_LCD_MISO  BOARD_LCD_MISO
#define PIN_LCD_CS    BOARD_LCD_CS
#define PIN_LCD_DC    BOARD_LCD_DC
#define PIN_LCD_RST   BOARD_LCD_RST
#define PIN_LCD_BL    BOARD_LCD_BL

#define LCD_H_RES  BOARD_LCD_H_RES
#define LCD_V_RES  BOARD_LCD_V_RES

/* ILI9341 厂家初始化序列 (gamma / power 时序, 与屏幕批次匹配) */
static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xCF, (uint8_t []){0x00, 0xC1, 0x30}, 3, 0},
    {0xED, (uint8_t []){0x64, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, (uint8_t []){0x85, 0x00, 0x78}, 3, 0},
    {0xCB, (uint8_t []){0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, (uint8_t []){0x20}, 1, 0},
    {0xEA, (uint8_t []){0x00, 0x00}, 2, 0},
    {0xC0, (uint8_t []){0x13}, 1, 0},
    {0xC1, (uint8_t []){0x13}, 1, 0},
    {0xC5, (uint8_t []){0x22, 0x35}, 2, 0},
    {0xC7, (uint8_t []){0xBD}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0xB1, (uint8_t []){0x00, 0x1B}, 2, 0},
    {0xB6, (uint8_t []){0x0A, 0xA2}, 2, 0},
    {0xF2, (uint8_t []){0x00}, 1, 0},
    {0x26, (uint8_t []){0x01}, 1, 0},
    {0xE0, (uint8_t []){0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7, 0x33, 0x07,
                        0x0F, 0x03, 0x0C, 0x0A, 0x00}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58, 0x4D, 0x07,
                        0x10, 0x0C, 0x32, 0x34, 0x0F}, 15, 0},
    {0, (uint8_t []){0}, 0x11, 0},
    {0, (uint8_t []){0}, 0x29, 0},
};

static esp_lcd_panel_io_handle_t s_panel_io = NULL;   /* LCD 面板 IO 句柄 (SPI 通道) */
static esp_lcd_panel_handle_t s_panel = NULL;         /* LCD 面板句柄 */
static int64_t s_slpo_us = 0;                         /* SLPOUT 发出时刻 (us) */

#define LCD_SLEEP_OUT_MS  120   /* 退出休眠后需等待的稳定时间 */

/* 前半段 (app_main 头部): 建 SPI/IO/面板 + 发 SLPOUT (记时刻), 非阻塞 */
esp_lcd_panel_handle_t lcd_init_early(spi_host_device_t host)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_LCD_SCLK,
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = PIN_LCD_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num       = PIN_LCD_CS,
        .dc_gpio_num       = PIN_LCD_DC,
        .spi_mode          = 0,
        .pclk_hz           = BOARD_LCD_SPI_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host,
                                              &io_config, &s_panel_io));

    const ili9341_vendor_config_t vendor_config = {
        .init_cmds      = vendor_specific_init,
        .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,          /* -1: 不接管复位 (与 EN 共用) */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_panel_io, &panel_config, &s_panel));

    /* SLPOUT: 退出休眠 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_panel_io, 0x11, NULL, 0));
    s_slpo_us = esp_timer_get_time();

    lcd_backlight_init();

    ESP_LOGI(TAG, "LCD early done (SLPOUT t=%lld us)", s_slpo_us);
    return s_panel;
}

/* 后半段 (lvgl 任务, 无限循环前): 距 SLPOUT ≥120ms 后初始化寄存器 + DISPON */
esp_lcd_panel_handle_t lcd_init_finish(void)
{
    int64_t remain_us = LCD_SLEEP_OUT_MS * 1000 - (esp_timer_get_time() - s_slpo_us);
    if (remain_us > 0) {
        vTaskDelay(pdMS_TO_TICKS((remain_us + 999) / 1000));
    }

    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    esp_lcd_panel_swap_xy(s_panel, BOARD_LCD_SWAP_XY);
    esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y);
    esp_lcd_panel_invert_color(s_panel, BOARD_LCD_INVERT);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "LCD init done (%dx%d)", LCD_H_RES, LCD_V_RES);
    return s_panel;
}

esp_lcd_panel_handle_t lcd_get_panel(void)
{
    return s_panel;
}

/* 同步刷屏: 排入 DMA 后通过一次无命令 tx_param 等待在途事务排空 */
esp_err_t lcd_draw_bitmap_sync(int x_start, int y_start, int x_end, int y_end,
                               const void *color_data)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, color_data),
                        TAG, "draw bitmap failed");
    return esp_lcd_panel_io_tx_param(s_panel_io, -1, NULL, 0);
}

/* ────────────────────────── Backlight (LEDC PWM) ────────────────────────── */
#define BL_PWM_FREQ_HZ  5000

/* 亮度感知曲线 (γ=2.2 对数映射): 0~255 → LEDC 占空比 */
static const uint8_t s_brightness_lut[256] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6,
    6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 11, 12,
    12, 13, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19,
    20, 20, 21, 22, 22, 23, 23, 24, 25, 25, 26, 26, 27, 28, 28, 29,
    30, 30, 31, 32, 33, 33, 34, 35, 35, 36, 37, 38, 39, 39, 40, 41,
    42, 43, 43, 44, 45, 46, 47, 48, 49, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    73, 74, 75, 76, 77, 78, 79, 81, 82, 83, 84, 85, 87, 88, 89, 90,
    91, 93, 94, 95, 97, 98, 99, 100, 102, 103, 105, 106, 107, 109, 110, 111,
    113, 114, 116, 117, 119, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133, 135,
    137, 138, 140, 141, 143, 145, 146, 148, 149, 151, 153, 154, 156, 158, 159, 161,
    163, 165, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 190,
    192, 194, 196, 197, 199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221,
    223, 225, 227, 229, 231, 234, 236, 238, 240, 242, 244, 246, 248, 251, 253, 255,
};

static void lcd_backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = BL_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
}

void lcd_set_brightness(uint8_t level)
{
    uint8_t duty = s_brightness_lut[level];
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_jd9853.h"
#include "drv_display.h"

static const char *TAG = "drv_display";

/* ────────────────────────── LCD (JD9853) ────────────────────────── */
#define PIN_LCD_SCLK  18
#define PIN_LCD_MOSI  23
#define PIN_LCD_CS     5
#define PIN_LCD_DC    32
#define PIN_LCD_RST   33
#define PIN_LCD_BL     4

#define LCD_H_RES  172
#define LCD_V_RES  320

static const jd9853_lcd_init_cmd_t init_cmds[] = {
    {0x11, (uint8_t []){ 0x00 }, 0, 120},
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    {0xB2, (uint8_t[]){0x23}, 1, 0},
    {0xB7, (uint8_t[]){0x00, 0x47, 0x00, 0x6F}, 4, 0},
    {0xBB, (uint8_t[]){0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0}, 6, 0},
    {0xC0, (uint8_t[]){0x44, 0xA4}, 2, 0},
    {0xC1, (uint8_t[]){0x16}, 1, 0},
    {0xC3, (uint8_t[]){0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77}, 8, 0},
    {0xC4, (uint8_t[]){0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82}, 12, 0},
    {0xC8, (uint8_t[]){0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
                       0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00}, 32, 0},
    {0xD0, (uint8_t[]){0x04, 0x06, 0x6B, 0x0F, 0x00}, 5, 0},
    {0xD7, (uint8_t[]){0x00, 0x30}, 2, 0},
    {0xE6, (uint8_t[]){0x14}, 1, 0},
    {0xDE, (uint8_t[]){0x01}, 1, 0},
    {0xB7, (uint8_t[]){0x03, 0x13, 0xEF, 0x35, 0x35}, 5, 0},
    {0xC1, (uint8_t[]){0x14, 0x15, 0xC0}, 3, 0},
    {0xC2, (uint8_t[]){0x06, 0x3A}, 2, 0},
    {0xC4, (uint8_t[]){0x72, 0x12}, 2, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xDE, (uint8_t[]){0x02}, 1, 0},
    {0xE5, (uint8_t[]){0x00, 0x02, 0x00}, 3, 0},
    {0xE5, (uint8_t[]){0x01, 0x02, 0x00}, 3, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x05}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x22, 0x00, 0xCD}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4, 0},
    {0xDE, (uint8_t[]){0x02}, 1, 0},
    {0xE5, (uint8_t[]){0x00, 0x02, 0x00}, 3, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0x29, (uint8_t []){ 0x00 }, 0, 0},
};

esp_lcd_panel_handle_t lcd_init(spi_host_device_t host)
{
    /* SPI bus */
    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_LCD_SCLK,
        .mosi_io_num     = PIN_LCD_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO));

    /* Panel IO */
    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num       = PIN_LCD_CS,
        .dc_gpio_num       = PIN_LCD_DC,
        .spi_mode          = 0,
        .pclk_hz           = 80 * 1000 * 1000,
        .trans_queue_depth = 1,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host,
                                              &io_config, &panel_io));

    /* Panel */
    jd9853_vendor_config_t vendor_cfg = {
        .init_cmds      = init_cmds,
        .init_cmds_size = sizeof(init_cmds) / sizeof(jd9853_lcd_init_cmd_t),
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(panel_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    esp_lcd_panel_set_gap(panel, 34, 0);
    esp_lcd_panel_mirror(panel, true, true);
    esp_lcd_panel_invert_color(panel, true);

    /* Backlight */
    gpio_config_t bl_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_conf));
    gpio_set_level(PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "LCD init done (%dx%d), gap=(34,0)", LCD_H_RES, LCD_V_RES);
    return panel;
}

/* ────────────────────────── Touch (CST816) ────────────────────────── */
#define TOUCH_I2C_PORT    I2C_NUM_0
#define TOUCH_SCL_IO      27
#define TOUCH_SDA_IO      26
#define TOUCH_ADDR        0x15

void touch_init(void)
{
    i2c_config_t conf = {
        .mode          = I2C_MODE_MASTER,
        .sda_io_num    = TOUCH_SDA_IO,
        .scl_io_num    = TOUCH_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, conf.mode, 0, 0, 0));

    /* wake up: write 0xFE = 0x01 (DisAutoSleep) */
    ESP_LOGI(TAG, "waking up touch...");
    uint8_t wake[] = {0xFE, 0x01};
    while (i2c_master_write_to_device(TOUCH_I2C_PORT, TOUCH_ADDR,
            wake, sizeof(wake), pdMS_TO_TICKS(50)) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "touch ready");
}

bool touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t reg = 0x01;
    uint8_t buf[7] = {0};

    esp_err_t ret = i2c_master_write_read_device(TOUCH_I2C_PORT, TOUCH_ADDR,
        &reg, 1, buf, sizeof(buf), pdMS_TO_TICKS(10));

    if (ret != ESP_OK || buf[1] == 0) {
        return false;
    }

    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

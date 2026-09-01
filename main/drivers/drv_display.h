#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 显示屏 (JD9853, SPI).
 * 两段式初始化: lcd_init_early 在 app_main 头部 (建 SPI/面板 + SLPOUT, 非阻塞),
 * lcd_init_finish 在 lvgl 任务无限循环前 (距 SLPOUT≥120ms 后发寄存器命令 + DISPON).
 * 硬件复位由 bootloader 完成. */
esp_lcd_panel_handle_t lcd_init_early(spi_host_device_t host);
esp_lcd_panel_handle_t lcd_init_finish(void);

/* 面板句柄 (LVGL 注册显示驱动用, 早于 lcd_init_finish) */
esp_lcd_panel_handle_t lcd_get_panel(void);

/* 背光亮度 (LEDC PWM, 0~255) */
void lcd_set_brightness(uint8_t level);

/* 触摸屏 (CST816, I2C) */
void touch_init(void);
bool touch_read(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif

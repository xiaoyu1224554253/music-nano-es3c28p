#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 显示屏 (JD9853, SPI) */
esp_lcd_panel_handle_t lcd_init(spi_host_device_t host);

/* 触摸屏 (CST816, I2C) */
void touch_init(void);
bool touch_read(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif

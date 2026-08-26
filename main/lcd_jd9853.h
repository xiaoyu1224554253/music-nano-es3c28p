#pragma once

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_lcd_panel_handle_t lcd_init(spi_host_device_t host);

#ifdef __cplusplus
}
#endif

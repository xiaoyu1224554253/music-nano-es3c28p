#include "board_i2c.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "board_i2c";

static i2c_master_bus_handle_t s_bus = NULL;

i2c_master_bus_handle_t board_i2c_bus(void)
{
    if (s_bus != NULL) return s_bus;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port           = BOARD_TOUCH_I2C_PORT,
        .sda_io_num         = BOARD_TOUCH_SDA,
        .scl_io_num         = BOARD_TOUCH_SCL,
        .clk_source         = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt  = 7,
        .intr_priority      = 0,
        .trans_queue_depth  = 0,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));
    ESP_LOGI(TAG, "I2C 总线就绪 (SDA=%d SCL=%d)", BOARD_TOUCH_SDA, BOARD_TOUCH_SCL);
    return s_bus;
}

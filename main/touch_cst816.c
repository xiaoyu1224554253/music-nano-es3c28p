#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "touch_cst816.h"

static const char *TAG = "touch";

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
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, conf.mode, 0, 0, 0));

    /* wake up: write 0xFE = 0x01 (DisAutoSleep) */
    /*ESP_LOGI(TAG, "waking up touch...");
    uint8_t wake[] = {0xFE, 0x01};
    while (i2c_master_write_to_device(TOUCH_I2C_PORT, TOUCH_ADDR,
            wake, sizeof(wake), pdMS_TO_TICKS(50)) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }*/
    ESP_LOGI(TAG, "touch ready");
}

bool touch_read(uint16_t *x, uint16_t *y)
{
    uint8_t reg = 0x01;
    uint8_t buf[7] = {0};

    esp_err_t ret = i2c_master_write_read_device(TOUCH_I2C_PORT, TOUCH_ADDR,
        &reg, 1, buf, sizeof(buf), pdMS_TO_TICKS(20));

    if (ret != ESP_OK || buf[1] == 0) {
        return false;
    }

    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

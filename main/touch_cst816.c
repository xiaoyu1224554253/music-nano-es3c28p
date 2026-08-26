#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "touch_cst816.h"
#include "esp_timer.h"
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



/*printf("buf[0]=%u, buf[1]=%u, buf[2]高4位=%u, buf[2]低4位=%u, buf[3]=%u, buf[4]高4位=%u, buf[4]低4位=%u, buf[5]=%u, buf[6]=%u\n",
       buf[0], 
       buf[1],
       (buf[2] >> 4) & 0x0F,
       buf[2] & 0x0F,
       buf[3],
       (buf[4] >> 4) & 0x0F,
       buf[4] & 0x0F,
       buf[5],
       buf[6]);
*/
    if (ret != ESP_OK || buf[1] == 0) {
        return false;
    }

    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

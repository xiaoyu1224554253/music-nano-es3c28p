#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "atomic_utils.h"
#include "touch_task.h"

#define TAG "touch"

#define TOUCH_I2C_PORT      I2C_NUM_0
#define TOUCH_SCL_IO        27
#define TOUCH_SDA_IO        26
#define TOUCH_ADDR          0x15

#define TOUCH_POLL_MS       20      /* 50Hz 轮询 */
#define TOUCH_CMD_TIMEOUT_MS 5      /* 短超时: 事务卡住也快速返回, 不拖累系统 */
#define TOUCH_DEAD_PROBE_MS 20     /* 离线后 10Hz 低频探测 */

/* 原子共享: LVGL 只读, 触摸任务只写 */
static volatile bool    s_present   = false;
static volatile int32_t s_x         = 0;
static volatile int32_t s_y         = 0;
static volatile bool    s_enabled   = true;
static volatile int32_t s_heartbeat = 0;

/* 从机在线探测: 只发 START + 地址 + STOP (最短事务), 从机 NACK 即认为离线 */
static bool touch_probe(void)
{
    i2c_cmd_handle_t link = i2c_cmd_link_create();
    if (!link) return false;

    i2c_master_start(link);
    i2c_master_write_byte(link, (TOUCH_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(link);
    esp_err_t ret = i2c_master_cmd_begin(TOUCH_I2C_PORT, link,
                                         pdMS_TO_TICKS(TOUCH_CMD_TIMEOUT_MS));
    i2c_cmd_link_delete(link);
    return (ret == ESP_OK);
}

/* 完整读取: 成功返回 true 并给出原始坐标 (换算/钳制由 LVGL 层做) */
static bool touch_read_raw(int32_t *x, int32_t *y)
{
    uint8_t reg = 0x01;
    uint8_t buf[7] = {0};

    esp_err_t ret = i2c_master_write_read_device(TOUCH_I2C_PORT, TOUCH_ADDR,
        &reg, 1, buf, sizeof(buf), pdMS_TO_TICKS(TOUCH_CMD_TIMEOUT_MS));
    if (ret != ESP_OK || buf[1] == 0) return false;

    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

static void touch_bus_init(void)
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
    ESP_LOGI(TAG, "bus ready");
}

/* 50Hz 触摸任务: 独占 I2C.
 * 在线: 每 20ms 完整读一次;
 * 读失败 (芯片休眠/无响应) → 进入低频探测态, 只 10Hz 探测, 不反复读死从机;
 * 从机被手指点醒后, 探测成功即恢复在线. 不做驱动重装 (事后重建救不了竞态卡死). */
static void touch_task(void *arg)
{
    (void)arg;
    touch_bus_init();

    bool    online = true;
    int64_t next_probe_us = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
        atomic_store_i32(&s_heartbeat, atomic_load_i32(&s_heartbeat) + 1);

        if (!atomic_load_bool(&s_enabled)) {
            atomic_store_bool(&s_present, false);
            continue;
        }

        if (online) {
            int32_t x, y;
            if (touch_read_raw(&x, &y)) {
                atomic_store_i32(&s_x, x);
                atomic_store_i32(&s_y, y);
                atomic_store_bool(&s_present, true);
            } else {
                atomic_store_bool(&s_present, false);
                online = false;
                next_probe_us = esp_timer_get_time() + (int64_t)TOUCH_DEAD_PROBE_MS * 1000;
                //ESP_LOGW(TAG, "触摸无响应, 进入低频探测 (10Hz)");
            }
        } else {
            int64_t now = esp_timer_get_time();
            if (now >= next_probe_us) {
                next_probe_us = now + (int64_t)TOUCH_DEAD_PROBE_MS * 1000;
                if (touch_probe()) {
                    online = true;
                    //ESP_LOGI(TAG, "触摸已恢复");
                }
            }
            atomic_store_bool(&s_present, false);
        }
    }
}

void touch_task_start(void)
{
    xTaskCreatePinnedToCore(touch_task, "touch", 2048, NULL, 1, NULL, 1);
}

void touch_task_set_enabled(bool en)
{
    atomic_store_bool(&s_enabled, en);
    if (!en) atomic_store_bool(&s_present, false);
}

bool touch_is_present(void)
{
    return atomic_load_bool(&s_present);
}

void touch_get_point(int32_t *x, int32_t *y)
{
    *x = atomic_load_i32(&s_x);
    *y = atomic_load_i32(&s_y);
}

uint32_t touch_get_heartbeat(void)
{
    return (uint32_t)atomic_load_i32(&s_heartbeat);
}

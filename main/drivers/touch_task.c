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

/* 触摸芯片 (CST816) I2C 配置 */
#define TOUCH_I2C_PORT      I2C_NUM_0   /* 使用的 I2C 外设 */
#define TOUCH_SCL_IO        27          /* SCL 引脚 */
#define TOUCH_SDA_IO        26          /* SDA 引脚 */
#define TOUCH_ADDR          0x15        /* CST816 7bit I2C 地址 */

#define TOUCH_POLL_MS       20      /* 50Hz 轮询周期 */
#define TOUCH_CMD_TIMEOUT_MS 5      /* 短超时: 事务卡住也快速返回, 不拖累系统 */
#define TOUCH_DEAD_PROBE_MS 20     /* 离线后 10Hz 低频探测周期 */

/* 原子共享: LVGL 只读, 触摸任务只写 */
static volatile bool    s_present   = false;   /* 当前是否有效触点 (原子) */
static volatile int32_t s_x         = 0;       /* 最近一次原始 X 坐标 (原子) */
static volatile int32_t s_y         = 0;       /* 最近一次原始 Y 坐标 (原子) */
static volatile bool    s_enabled   = true;    /* 触摸采样开关 (息屏时 false) */
static volatile int32_t s_heartbeat = 0;       /* 心跳计数: 每次轮询 +1, LVGL 用于监视任务存活 */

/* 从机在线探测: 只发 START + 地址 + STOP (最短事务), 从机 NACK 即认为离线.
 * 返回 true=从机应答在线, false=离线 */
static bool touch_probe(void)
{
    i2c_cmd_handle_t link = i2c_cmd_link_create();   /* 创建命令链接 */
    if (!link) return false;

    i2c_master_start(link);
    i2c_master_write_byte(link, (TOUCH_ADDR << 1) | I2C_MASTER_WRITE, true);   /* 写地址位, 期待 ACK */
    i2c_master_stop(link);
    esp_err_t ret = i2c_master_cmd_begin(TOUCH_I2C_PORT, link,
                                         pdMS_TO_TICKS(TOUCH_CMD_TIMEOUT_MS)); /* 短超时执行 */
    i2c_cmd_link_delete(link);
    return (ret == ESP_OK);   /* ACK 到位 = 在线 */
}

/* 完整读取: 成功返回 true 并给出原始坐标 (换算/钳制由 LVGL 层做).
 * 寄存器 0x01 起读 7 字节: [状态, x高, x低, x高, x低, ..., 压力]. */
static bool touch_read_raw(int32_t *x, int32_t *y)
{
    uint8_t reg = 0x01;
    uint8_t buf[7] = {0};

    esp_err_t ret = i2c_master_write_read_device(TOUCH_I2C_PORT, TOUCH_ADDR,
        &reg, 1, buf, sizeof(buf), pdMS_TO_TICKS(TOUCH_CMD_TIMEOUT_MS));
    if (ret != ESP_OK || buf[1] == 0) return false;   /* 读失败或触点有效标志=0 → 无触点 */

    /* 坐标按 CST816 格式拼接: 12bit, 高 4 位在第二字节低半字节 */
    *x = ((buf[2] & 0x0F) << 8) | buf[3];
    *y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

/* I2C 总线初始化: 400kHz 主模式, 使能内部上拉 */
static void touch_bus_init(void)
{
    i2c_config_t conf = {
        .mode          = I2C_MODE_MASTER,
        .sda_io_num    = TOUCH_SDA_IO,
        .scl_io_num    = TOUCH_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,          /* 400k 标准快速模式 */
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, conf.mode, 0, 0, 0));   /* 不配接收/发送缓冲 */
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

    bool    online = true;           /* 从机当前是否判定为在线 */
    int64_t next_probe_us = 0;       /* 下次探测时刻 (us) */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));               /* 20ms 周期 */
        atomic_store_i32(&s_heartbeat, atomic_load_i32(&s_heartbeat) + 1);   /* 心跳+1 */

        if (!atomic_load_bool(&s_enabled)) {   /* 息屏期间不读 I2C */
            atomic_store_bool(&s_present, false);   /* 强制无触点, 防止残留触点卡住界面 */
            continue;
        }

        if (online) {
            int32_t x, y;
            if (touch_read_raw(&x, &y)) {       /* 在线: 正常读坐标 */
                atomic_store_i32(&s_x, x);
                atomic_store_i32(&s_y, y);
                atomic_store_bool(&s_present, true);
            } else {                            /* 读失败 → 转入低频探测态 */
                atomic_store_bool(&s_present, false);
                online = false;
                next_probe_us = esp_timer_get_time() + (int64_t)TOUCH_DEAD_PROBE_MS * 1000;
                //ESP_LOGW(TAG, "触摸无响应, 进入低频探测 (10Hz)");
            }
        } else {
            /* 离线态: 只在到点那一刻做一次最短探测, 其余轮询直接跳过 */
            int64_t now = esp_timer_get_time();
            if (now >= next_probe_us) {
                next_probe_us = now + (int64_t)TOUCH_DEAD_PROBE_MS * 1000;
                if (touch_probe()) {
                    online = true;              /* 探测到 ACK → 恢复在线 */
                    //ESP_LOGI(TAG, "触摸已恢复");
                }
            }
            atomic_store_bool(&s_present, false);
        }
    }
}

/* 启动触摸任务 (固定 core 1, 优先级 1) */
void touch_task_start(void)
{
    xTaskCreatePinnedToCore(touch_task, "touch", 2048, NULL, 1, NULL, 1);
}

/* 启停触摸采样: en=true 使能, false 停止并清除触点状态 (息屏用) */
void touch_task_set_enabled(bool en)
{
    atomic_store_bool(&s_enabled, en);
    if (!en) atomic_store_bool(&s_present, false);
}

/* 查询当前是否有有效触点 (原子读) */
bool touch_is_present(void)
{
    return atomic_load_bool(&s_present);
}

/* 读取最近一次原始坐标: x/y=输出参数 */
void touch_get_point(int32_t *x, int32_t *y)
{
    *x = atomic_load_i32(&s_x);
    *y = atomic_load_i32(&s_y);
}

/* 读取心跳计数 (LVGL 检测触摸任务是否停摆) */
uint32_t touch_get_heartbeat(void)
{
    return (uint32_t)atomic_load_i32(&s_heartbeat);
}

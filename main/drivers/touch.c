#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "board_config.h"
#include "board_i2c.h"
#include "touch.h"

#define TAG "touch"

/* 触摸芯片 (FT6336G): 与音频 codec 共用板载 I2C 总线 */
#define TOUCH_ADDR          BOARD_TOUCH_ADDR
#define TOUCH_RST_IO        BOARD_TOUCH_RST
#define TOUCH_INT_IO        BOARD_TOUCH_INT

#define TOUCH_CMD_TIMEOUT_MS 5          /* 短超时: 事务卡住也快速返回, 不拖累 LVGL */

static i2c_master_dev_handle_t s_dev = NULL;   /* FT6336 I2C 设备句柄 */
static volatile bool s_int_flag = false;       /* INT 下降沿标志 (ISR 置位, 任务清) */
static bool          s_present  = false;       /* 当前是否有有效触点 (仅任务写) */
static int32_t       s_x        = 0;           /* 最近一次原始 X 坐标 */
static int32_t       s_y        = 0;           /* 最近一次原始 Y 坐标 */

/* GPIO INT ISR: 只锁存标志 */
static void IRAM_ATTR touch_int_isr(void *arg)
{
    (void)arg;
    s_int_flag = true;
}

/* 完整读取: 成功返回 true 并给出原始坐标 (换算/钳制由 LVGL 层做).
 * FT6336: 寄存器 0x02 起读 5 字节 [点数, x高, x低, y高, y低] (12bit 坐标). */
static bool touch_read_raw(int32_t *x, int32_t *y)
{
    uint8_t reg = 0x02;
    uint8_t buf[5] = {0};

    if (s_dev == NULL) return false;
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf),
                                                pdMS_TO_TICKS(TOUCH_CMD_TIMEOUT_MS));
    if (ret != ESP_OK) return false;

    if ((buf[0] & 0x0F) == 0) return false;   /* 无触点 */

    *x = ((buf[1] & 0x0F) << 8) | buf[2];
    *y = ((buf[3] & 0x0F) << 8) | buf[4];
    return true;
}

/* 硬件复位 FT6336 (低有效脉冲) */
static void touch_hw_reset(void)
{
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << TOUCH_RST_IO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(TOUCH_RST_IO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));   /* 复位后需稳定时间 */
}

/* 初始化: 硬件复位 + 挂到共享 I2C 总线 + INT 下降沿中断 */
void touch_init(void)
{
    touch_hw_reset();

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TOUCH_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(board_i2c_bus(), &dev_cfg, &s_dev));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << TOUCH_INT_IO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPIO ISR 服务安装失败: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_INT_IO, touch_int_isr, NULL));

    s_int_flag = false;
    s_present  = false;
    ESP_LOGI(TAG, "触摸就绪 FT6336 (addr=0x%02X INT=%d RST=%d)",
             TOUCH_ADDR, TOUCH_INT_IO, TOUCH_RST_IO);
}

/* 读触点: 有中断或当前判定按下时才读 I2C */
bool touch_poll(int32_t *x, int32_t *y)
{
    if (s_int_flag || s_present) {
        s_int_flag = false;
        int32_t nx, ny;
        if (touch_read_raw(&nx, &ny)) {
            s_x = nx;
            s_y = ny;
            s_present = true;
        } else {
            s_present = false;
        }
    }

    if (s_present) {
        *x = s_x;
        *y = s_y;
    }
    return s_present;
}

/* 清触点/中断状态 (息屏时调用) */
void touch_reset(void)
{
    s_int_flag = false;
    s_present  = false;
}

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 触摸任务 (50Hz, 独占 I2C):
 * 在线时每 20ms 完整读取; 读失败(芯片休眠/无响应)后进入低频探测态,
 * 只做 10Hz 探测, 不反复读死从机 (规避 ESP32 原版 I2C FSM 卡死竞态).
 * LVGL 侧只读原子量, 永不直接操作 I2C. */

void    touch_task_start(void);
void    touch_task_set_enabled(bool en);      /* 息屏时 false: 停止读 I2C */
bool    touch_is_present(void);                /* 原子读: 当前是否有效触点 */
void    touch_get_point(int32_t *x, int32_t *y); /* 原子读: 原始坐标 */
uint32_t touch_get_heartbeat(void);            /* 心跳: LVGL 检测任务是否停摆 */

#ifdef __cplusplus
}
#endif

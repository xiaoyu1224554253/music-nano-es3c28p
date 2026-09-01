#ifndef __POWER_MGR_H__
#define __POWER_MGR_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 电源管理: GPIO37 息屏/唤醒按键 + 深度睡眠 (GPIO22 切断外设供电).
 * power_mgr_early_init 在 app_main 第一行调用 (LCD 前半段之前, 开启外设供电);
 * power_mgr_init 在 LVGL 任务中调用; 按键由 vol_key 定时器复用, 10ms 采样调 power_mgr_poll_key */
void power_mgr_early_init(void);
void power_mgr_init(void);

/* 外部亮度变化 (亮度滑块) 时同步当前亮度, 保证淡入/淡出起点正确.
 * v=新的当前亮度 (0~255) */
void power_mgr_set_cur_bri(uint8_t v);

/* 按键电平上报 (上升沿触发, 调用方每 10ms 上报一次当前电平).
 * level=按键当前电平 (true=按下) */
void power_mgr_poll_key(bool level);

#ifdef __cplusplus
}
#endif

#endif

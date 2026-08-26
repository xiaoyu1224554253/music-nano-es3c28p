#ifndef __UI_CORE_H__
#define __UI_CORE_H__

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "bt_a2dp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕尺寸 */
#define TFT_HOR_RES   172
#define TFT_VER_RES   320

/* 配色 (播放器主界面 / 各 UI 组件共用) */
#define COLOR_BG      lv_color_hex(0x050505)
#define COLOR_CARD    lv_color_hex(0x111111)
#define COLOR_FG      lv_color_hex(0xF0F0F0)
#define COLOR_MUTED   lv_color_hex(0x888888)
#define COLOR_ACCENT  lv_color_hex(0x00D992)
#define COLOR_BORDER  lv_color_hex(0x2A2A2A)
#define COLOR_DIM     lv_color_hex(0x555555)

/* UI 共享句柄 (由 ui_core_init 从参数装配) */
extern QueueHandle_t        g_ui_app_cmd_queue;
extern bt_a2dp_iface_t     *g_ui_bt_iface;
extern QueueHandle_t        g_ui_audio_cmd_queue;
extern QueueHandle_t        g_ui_audio_rsp_queue;

typedef struct {
    QueueHandle_t        app_cmd_queue;
    bt_a2dp_iface_t     *bt_iface;
    QueueHandle_t        audio_cmd_queue;
    QueueHandle_t        audio_rsp_queue;
} ui_params_t;

/* 通用圆形图标按钮 (透明底 + 灰色符号), 供主界面/组件创建控件 */
lv_obj_t *make_icon_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, const char *symbol);

/* LVGL 初始化: 装配共享句柄 + 创建主任务 (运行 ui_loop_task) */
void ui_core_init(const ui_params_t *params);

/* 显示/输入驱动初始化 (在 ui_loop_task 内调用) */
void ui_core_display_init(void);

/* LVGL 主循环 (ui_core_init 创建的任务主体, 定义于 ui_loop.c) */
void ui_loop_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif

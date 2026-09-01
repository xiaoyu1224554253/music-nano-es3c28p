#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "lvgl.h"
#include "drv_display.h"
#include "touch_task.h"
#include "settings.h"
#include "bt_a2dp.h"
#include "ui_core.h"
#include "power_mgr.h"

static const char *TAG = "POWER";

/* GPIO37: 息屏/唤醒按键 (外部 10k 下拉, 仅输入, 只测上升沿) */
#define PIN_PWR_KEY      GPIO_NUM_37
/* GPIO22: 外设供电控制 (高=切断外设供电), 深睡时保持 */
#define PIN_PWR_CUT      GPIO_NUM_22

/* 息屏时 LVGL 刷新周期: 100 秒一次 (省电, 屏幕已黑无需频繁刷新) */
#define LVGL_SCREEN_OFF_REFRESH_MS  100000

/* 淡出/淡入: 变化幅度 1~255 → 总时长 50~400ms (线性映射, 避免拉满后过长) */
#define BRI_FADE_TIME_MIN_MS  50
#define BRI_FADE_TIME_MAX_MS  400

/* 按键冷却: 吞机械抖动产生的二次上升沿 */
#define PWR_KEY_COOLDOWN_MS  200

/* 电源状态机 */
typedef enum {
    PWR_ACTIVE,       /* 正常: 亮屏 */
    PWR_SCREEN_OFF,   /* 手动息屏 (BT 保持连接, 音频继续) */
    PWR_FAKE_OFF,     /* 淡出中(即将深睡), 忽略按键 */
} pwr_state_t;

static pwr_state_t s_state = PWR_ACTIVE;   /* 当前电源状态 */
static bool        s_key_prev = false;     /* 上次按键电平 (用于检测上升沿) */
static int64_t     s_key_last_us = 0;      /* 上次按键沿的时刻 (us), 用于冷却 */
static uint8_t     s_cur_bri = 0;          /* 当前实际背光亮度 */
static int         s_fade_dummy = 0;       /* 动画变量占位 (值由 bri_fade_exec 使用) */

/* 淡出/淡入执行: 直接写 LEDC 背光.
 * var=动画变量地址(未用), v=动画当前值 (目标亮度) */
static void bri_fade_exec(void *var, int32_t v)
{
    (void)var;
    s_cur_bri = (uint8_t)v;
    lcd_set_brightness(s_cur_bri);
}

/* 外部亮度变化 (亮度滑块) 时同步当前亮度, 保证下次淡入/淡出起点正确.
 * v=新的当前亮度 */
void power_mgr_set_cur_bri(uint8_t v)
{
    s_cur_bri = v;
}

/* 淡入/淡出时长: 变化幅度 diff → 50~400ms 线性映射 (diff=0 不淡).
 * diff=亮度变化量 (0~255) */
static int bri_fade_time_ms(int diff)
{
    if (diff <= 0) return 1;
    if (diff > 255) diff = 255;
    return BRI_FADE_TIME_MIN_MS + (diff - 1) * (BRI_FADE_TIME_MAX_MS - BRI_FADE_TIME_MIN_MS) / (255 - 1);
}

/* LVGL 刷新频率: slow=true 降为 100s 一次, false 恢复 20ms */
static void set_lvgl_slow_refresh(bool slow)
{
    lv_timer_t *refr = _lv_disp_get_refr_timer(lv_disp_get_default());   /* 拿 LVGL 刷新定时器 */
    if (!refr) return;
    if (slow) {
        lv_timer_set_period(refr, LVGL_SCREEN_OFF_REFRESH_MS);   /* 息屏: 刷新周期拉长到 100s */
    } else {
        lv_timer_set_period(refr, LV_DISP_DEF_REFR_PERIOD);      /* 亮屏: 恢复默认周期 */
        lv_timer_resume(refr);                                   /* 确保定时器处于运行态 */
    }
}

/* 淡到目标亮度: 时长 = 变化幅度映射到 50~400ms; done 为动画完成回调.
 * target=目标亮度, done=完成后回调 (如 enter_deep_sleep) */
static void screen_fade(uint8_t target, lv_anim_ready_cb_t done)
{
    int diff = (int)s_cur_bri - (int)target;   /* 变化幅度 */
    if (diff < 0) diff = -diff;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_fade_dummy);                       /* 动画对象 */
    lv_anim_set_exec_cb(&a, bri_fade_exec);                   /* 每帧执行: 写背光 */
    lv_anim_set_values(&a, s_cur_bri, target);                /* 起始→目标亮度 */
    lv_anim_set_time(&a, bri_fade_time_ms(diff));             /* 时长按幅度映射 */
    lv_anim_set_path_cb(&a, lv_anim_path_linear);             /* 线性插值 */
    if (done) lv_anim_set_ready_cb(&a, done);                 /* 完成回调 */
    lv_anim_start(&a);
}

/* 进入深度睡眠: GPIO22 拉高+保持(切断外设供电), GPIO37 高电平唤醒 (不返回) */
static void enter_deep_sleep(lv_anim_t *a)
{
    (void)a;
    ESP_LOGI(TAG, "进入深度睡眠 (GPIO22 高电平保持, GPIO37 高电平唤醒)");

    /* 切外设供电: GPIO22 输出高 + 保持电平 (深睡中 GPIO 状态丢失, 靠 hold 保持) */
    gpio_config_t cut_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_PWR_CUT,
    };
    gpio_config(&cut_cfg);
    gpio_set_level(PIN_PWR_CUT, 1);        /* 高 = 断电 */
    gpio_hold_en(PIN_PWR_CUT);             /* 锁定引脚电平, 深睡期间不掉 */
    gpio_deep_sleep_hold_en();             /* 允许深睡期间保持引脚 */

    esp_sleep_enable_ext0_wakeup(PIN_PWR_KEY, 1);   /* GPIO37 高电平唤醒 */
    esp_deep_sleep_start();                          /* 进入深睡 (此处不返回) */
}

/* 息屏: 淡出 + 降刷新 + 关触摸 (BT 保持连接) */
static void screen_off(void)
{
    s_state = PWR_SCREEN_OFF;
    screen_fade(0, NULL);                    /* 背光淡出到 0 */
    set_lvgl_slow_refresh(true);             /* 刷新降频省电 */
    ui_touch_set_enabled(false);             /* LVGL 触摸开关置 off */
    touch_task_set_enabled(false);           /* 触摸任务也停读 I2C */
    ESP_LOGI(TAG, "息屏 (BT 保持连接)");
}

/* 亮屏: 恢复刷新+触摸 → 淡入 */
static void screen_on(void)
{
    s_state = PWR_ACTIVE;
    set_lvgl_slow_refresh(false);            /* 恢复刷新频率 */
    ui_touch_set_enabled(true);              /* 恢复触摸 */
    touch_task_set_enabled(true);
    screen_fade(brightness_get(), NULL);     /* 淡入到保存的亮度 */
    ESP_LOGI(TAG, "亮屏");
}

/* 按键电平上报 (复用 vol_key 10ms 定时器): 只测上升沿, 按住不重复触发.
 * level=当前按键电平 */
void power_mgr_poll_key(bool level)
{
    bool rising = level && !s_key_prev;   /* 上升沿: 之前低现在高 */
    s_key_prev = level;
    if (!rising) return;

    /* 冷却窗口: 两次沿间隔 <200ms 视为抖动, 丢弃 */
    int64_t now = esp_timer_get_time();
    if (s_key_last_us != 0 && (now - s_key_last_us) < PWR_KEY_COOLDOWN_MS * 1000LL) {
        return;   /* 冷却: 吞机械抖动二次沿 */
    }
    s_key_last_us = now;

    switch (s_state) {
    case PWR_ACTIVE:
        if (bt_a2dp_get_state() != BT_STATE_DISCONNECTED) {
            /* 已连接/连接中 → 手动息屏 */
            screen_off();
        } else {
            /* 蓝牙未连接 → 淡出后进深度睡眠 */
            s_state = PWR_FAKE_OFF;
            ESP_LOGI(TAG, "BT 未连接, 淡出后深睡");
            screen_fade(0, enter_deep_sleep);
        }
        break;
    case PWR_SCREEN_OFF:
        screen_on();   /* 息屏中按键 → 亮屏 */
        break;
    case PWR_FAKE_OFF:
        break;   /* 深睡淡出中: 忽略按键 */
    }
}

/* 息屏期 BT 状态轮询 (1s): 从已连/连接中变为未连接 → 深睡 */
static void bt_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_state != PWR_SCREEN_OFF) return;   /* 非息屏态不处理 */
    if (bt_a2dp_get_state() == BT_STATE_DISCONNECTED) {
        ESP_LOGI(TAG, "息屏期间蓝牙断开, 进入深度睡眠");
        s_state = PWR_FAKE_OFF;
        enter_deep_sleep(NULL);
    }
}

/* 早启 (app_main 第一行, LCD 前半段之前): 释放深睡保持 + GPIO22 拉低开启外设供电.
 * 冷启动时 bootloader 已拉低 IO22, 此处为深睡唤醒兜底 (唤醒后 hold 仍保持高电平). */
void power_mgr_early_init(void)
{
    gpio_deep_sleep_hold_dis();          /* 关闭全局深睡引脚保持, 允许重新配置 */
    gpio_hold_dis(PIN_PWR_CUT);          /* 释放 GPIO22 锁定 */
    gpio_config_t cut_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_PWR_CUT,
    };
    gpio_config(&cut_cfg);
    gpio_set_level(PIN_PWR_CUT, 0);      /* 低 = 外设供电开启 */
}

/* 电源管理初始化 (在 LVGL 任务中调用):
 * 配置按键输入 + 开机背光渐入 + 启动 BT 状态轮询 */
void power_mgr_init(void)
{
    /* GPIO37 按键输入 (外部已有 10k 下拉, ESP32 输入脚无内部上下拉) */
    gpio_config_t key_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PIN_PWR_KEY,
    };
    gpio_config(&key_cfg);

    /* 初始电平作为上次值: 开机时按钮若仍按住, 需等一次释放后再按才触发 */
    s_key_prev = (gpio_get_level(PIN_PWR_KEY) == 1);
    s_key_last_us = 0;

    /* 开机背光渐入: 从 0 渐升到保存亮度 (复用 screen_fade) */
    s_cur_bri = 0;
    lcd_set_brightness(0);
    screen_fade(brightness_get(), NULL);

    /* 息屏期 BT 状态轮询: 1 秒一次 */
    lv_timer_create(bt_poll_cb, 1000, NULL);
}

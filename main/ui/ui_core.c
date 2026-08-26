#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "drv_display.h"
#include "ui_core.h"
#include "settings.h"

#define DRAW_BUF_SIZE  (TFT_HOR_RES * TFT_VER_RES / 10 * 7)
#define LVGL_BUF_SIZE  (DRAW_BUF_SIZE / 2)

#define TOUCH_Y_MIN  5
#define TOUCH_Y_MAX  310

/* UI 共享句柄 */
QueueHandle_t        g_ui_app_cmd_queue  = NULL;
bt_a2dp_iface_t     *g_ui_bt_iface       = NULL;
QueueHandle_t        g_ui_audio_cmd_queue = NULL;
QueueHandle_t        g_ui_audio_rsp_queue = NULL;

static lv_color_t           s_draw_buf1[LVGL_BUF_SIZE];
static lv_color_t           s_draw_buf2[LVGL_BUF_SIZE];
static lv_disp_draw_buf_t   s_draw_buf_dsc;
static lv_disp_drv_t        s_disp_drv;
static lv_indev_drv_t       s_indev_drv;   /* LVGL 8 只存指针不拷贝, 必须常驻 */
static esp_lcd_panel_handle_t s_panel = NULL;

static void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                          lv_color_t *color_p)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(disp_drv);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t tx, ty;
    if (touch_read(&tx, &ty)) {
        ty = (uint16_t)(((int)ty - TOUCH_Y_MIN) * (TFT_VER_RES - 1)
                        / (TOUCH_Y_MAX - TOUCH_Y_MIN));
        tx = TFT_HOR_RES - 1 - tx;
        ty = TFT_VER_RES - 1 - ty;
        if (tx >= TFT_HOR_RES) tx = TFT_HOR_RES - 1;
        if (ty >= TFT_VER_RES) ty = TFT_VER_RES - 1;
        data->point.x = tx;
        data->point.y = ty;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

lv_obj_t *make_icon_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h,
                        const char *symbol)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, COLOR_MUTED, 0);
    lv_obj_center(icon);

    return btn;
}

void ui_core_display_init(void)
{
    lv_init();

    s_panel = lcd_init(SPI2_HOST);
    touch_init();

    lv_disp_draw_buf_init(&s_draw_buf_dsc, s_draw_buf1, s_draw_buf2, LVGL_BUF_SIZE);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = TFT_HOR_RES;
    s_disp_drv.ver_res  = TFT_VER_RES;
    s_disp_drv.flush_cb = my_disp_flush;
    s_disp_drv.draw_buf = &s_draw_buf_dsc;
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&s_indev_drv);
}

void ui_core_init(const ui_params_t *params)
{
    g_ui_app_cmd_queue  = params->app_cmd_queue;
    g_ui_bt_iface       = params->bt_iface;
    g_ui_audio_cmd_queue = params->audio_cmd_queue;
    g_ui_audio_rsp_queue = params->audio_rsp_queue;

    volume_load_from_nvs();

    xTaskCreatePinnedToCore(ui_loop_task, "lvgl", 8192, NULL, 1, NULL, 1);
}

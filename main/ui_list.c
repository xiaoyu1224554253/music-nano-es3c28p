#include <string.h>
#include <stdio.h>
#include "sys_monitor.h"
#include "ui_list.h"
#include "lvgl_task.h"
#include "File.h"

extern const lv_font_t chinese_16;

#define FS_W        135
#define FS_H        220
#define FS_X        0
#define FS_Y        3
#define FS_ITEMS    30
#define FS_ROW_H    18

static lv_obj_t  *s_fs_overlay   = NULL;
static lv_obj_t  *s_fs_cont      = NULL;
static lv_obj_t  *s_fs_title     = NULL;
static lv_obj_t  *s_fs_list      = NULL;
static lv_obj_t  *s_fs_page_lbl  = NULL;
static lv_obj_t  *s_fs_prev_btn  = NULL;
static lv_obj_t  *s_fs_next_btn  = NULL;

static int   s_fs_page            = 0;
static int   s_fs_total           = 0;
static bool  s_fs_inside          = false;
static const char *s_fs_group    = NULL;
static lv_obj_t *s_fs_current_btn = NULL; /* 当前播放歌曲对应的列表行按钮 */

static lv_img_dsc_t s_fs_icon_dir;
static lv_img_dsc_t s_fs_icon_music;

static void (*s_play_cb)(const char *group, const char *name) = NULL;

void fs_list_set_play_cb(void (*cb)(const char *group, const char *name))
{
    s_play_cb = cb;
}

static void fs_browser_open(void);
static void fs_browser_close(void);
static void fs_browser_show_page(int page);
static void fs_browser_enter_dir(const char *cache_name);
static void fs_browser_go_back(void);
static void fs_browser_jump_to_current(void);

static int fs_cache_count_for_group(const char *group)
{
    if (!g_fs_cache || !group) return 0;
    int count = 0;
    for (int i = 0; i < g_fs_cache->count; i++) {
        if (strcmp(g_fs_cache->entries[i].group, group) == 0) count++;
    }
    return count;
}

static fs_entry_t *fs_cache_entry_for_group(const char *group, int idx)
{
    if (!g_fs_cache || !group) return NULL;
    int seen = 0;
    for (int i = 0; i < g_fs_cache->count; i++) {
        if (strcmp(g_fs_cache->entries[i].group, group) == 0) {
            if (seen == idx) return &g_fs_cache->entries[i];
            seen++;
        }
    }
    return NULL;
}

static void fs_item_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    fs_entry_t *entry = (fs_entry_t *)lv_obj_get_user_data(btn);
    if (!entry) return;

    if (entry->is_dir) {
        fs_browser_enter_dir(entry->name);
    } else if (s_play_cb) {
        /* 播放切换后由 fs_browser_refresh() 统一重绘, 让绿色高亮跟随新播放的歌曲 */
        s_play_cb(s_fs_group, entry->name);
    }
}

/* 该列表项是否对应主界面当前正在播放的歌曲 */
static bool fs_entry_is_current(fs_entry_t *entry)
{
    const char *group = player_current_group();
    const char *name  = player_current_name();
    if (!group || !name || !entry) return false;

    /* 文件条目: 组与文件名都匹配 */
    if (!entry->is_dir && s_fs_group
        && strcmp(s_fs_group, group) == 0
        && strcmp(entry->name, name) == 0) {
        return true;
    }

    /* 根目录下: 高亮包含当前播放歌曲的文件夹 */
    if (entry->is_dir && s_fs_group
        && strcmp(s_fs_group, "sdcard") == 0
        && strncmp(group, "sdcard_", 7) == 0
        && strcmp(entry->name, group + 7) == 0) {
        return true;
    }

    return false;
}

static void fs_add_item(fs_entry_t *entry)
{
    lv_img_dsc_t *icon = entry->is_dir ? &s_fs_icon_dir : &s_fs_icon_music;

    lv_obj_t *btn = lv_list_add_btn(s_fs_list, icon, entry->name);
    lv_obj_set_height(btn, 30);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_pad_top(btn, 3, 0);

    lv_obj_t *label = lv_obj_get_child(btn, 1);
    lv_obj_set_style_text_font(label, &chinese_16, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_height(label, lv_font_get_line_height(&chinese_16));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_set_user_data(btn, entry);
    lv_obj_add_event_cb(btn, fs_item_click_cb, LV_EVENT_CLICKED, NULL);

    /* 高亮当前正在播放的歌曲行 */
    if (fs_entry_is_current(entry)) {
        s_fs_current_btn = btn;
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xB7F7C2), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    }
}

static void fs_browser_show_page(int page)
{
    s_fs_page = page;
    s_fs_current_btn = NULL;

    int total = fs_cache_count_for_group(s_fs_group);
    s_fs_total = (total + FS_ITEMS - 1) / FS_ITEMS;
    if (s_fs_total < 1) s_fs_total = 1;

    int start = page * FS_ITEMS;
    int end   = (start + FS_ITEMS) < total ? (start + FS_ITEMS) : total;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d", page + 1, s_fs_total);
    lv_label_set_text(s_fs_page_lbl, buf);

    if (page <= 0) {
        lv_obj_add_state(s_fs_prev_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_fs_prev_btn, LV_STATE_DISABLED);
    }
    if (page >= s_fs_total - 1) {
        lv_obj_add_state(s_fs_next_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_fs_next_btn, LV_STATE_DISABLED);
    }

    lv_obj_clean(s_fs_list);

    if (total == 0) {
        lv_obj_t *btn = lv_list_add_btn(s_fs_list, NULL, "无文件");
        lv_obj_set_height(btn, 30);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_pad_top(btn, 3, 0);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        lv_obj_set_style_text_font(label, &chinese_16, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        return;
    }

    for (int i = start; i < end; i++) {
        fs_entry_t *entry = fs_cache_entry_for_group(s_fs_group, i);
        if (entry) fs_add_item(entry);
    }
}

static void fs_prev_click_cb(lv_event_t *e)
{
    if (s_fs_page > 0) {
        fs_browser_show_page(s_fs_page - 1);
    }
}

static void fs_next_click_cb(lv_event_t *e)
{
    if (s_fs_page < s_fs_total - 1) {
        fs_browser_show_page(s_fs_page + 1);
    }
}

static void fs_browser_close(void)
{
    if (s_fs_overlay) {
        lv_obj_del(s_fs_overlay);
    }
    s_fs_overlay   = NULL;
    s_fs_cont      = NULL;
    s_fs_title     = NULL;
    s_fs_list      = NULL;
    s_fs_page_lbl  = NULL;
    s_fs_prev_btn  = NULL;
    s_fs_next_btn  = NULL;
    s_fs_group     = NULL;
    s_fs_inside    = false;
    s_fs_current_btn = NULL;
}

static void fs_overlay_click_cb(lv_event_t *e)
{
    fs_browser_close();
}

static void fs_browser_enter_dir(const char *cache_name)
{
    s_fs_inside = true;

    static char group_buf[FS_GROUP_MAX];
    snprintf(group_buf, sizeof(group_buf), "sdcard_%s", cache_name);
    s_fs_group = group_buf;

    lv_label_set_text(s_fs_title, cache_name);

    fs_browser_show_page(0);
}

static void fs_browser_go_back(void)
{
    s_fs_inside = false;
    s_fs_group  = "sdcard";

    lv_label_set_text(s_fs_title, "Music Files");
    fs_browser_show_page(0);
}

static void fs_back_click_cb(lv_event_t *e)
{
    fs_browser_go_back();
}
static void fs_browser_open(void)
{
    if (s_fs_overlay) {
        fs_browser_close();
    }
    s_fs_icon_dir.header.w = 16;
    s_fs_icon_dir.header.h = 21;
    s_fs_icon_dir.data_size = 16 * 21 * 2;
    s_fs_icon_dir.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_fs_icon_dir.data = (const uint8_t *)file[0];
    s_fs_icon_music.header.w = 16;
    s_fs_icon_music.header.h = 21;
    s_fs_icon_music.data_size = 16 * 21 * 2;
    s_fs_icon_music.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_fs_icon_music.data = (const uint8_t *)file[1];

    s_fs_overlay = lv_btn_create(lv_scr_act());
    lv_obj_set_size(s_fs_overlay, 172, 320);
    lv_obj_set_pos(s_fs_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_fs_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_fs_overlay, 0, 0);
    lv_obj_set_style_shadow_width(s_fs_overlay, 0, 0);
    lv_obj_add_event_cb(s_fs_overlay, fs_overlay_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(s_fs_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_fs_cont = lv_obj_create(s_fs_overlay);
    lv_obj_set_pos(s_fs_cont, FS_X, FS_Y);
    lv_obj_set_size(s_fs_cont, FS_W, FS_H);
    lv_obj_set_style_bg_color(s_fs_cont, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_fs_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_fs_cont, 4, 0);
    lv_obj_set_style_border_color(s_fs_cont, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_width(s_fs_cont, 1, 0);
    lv_obj_set_style_pad_all(s_fs_cont, 0, 0);
    lv_obj_clear_flag(s_fs_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_fs_title = lv_label_create(s_fs_cont);
    lv_obj_set_pos(s_fs_title, 4, 0);
    lv_obj_set_size(s_fs_title, FS_W - 40, 22);
    lv_obj_set_style_text_font(s_fs_title, &chinese_16, 0);
    lv_obj_set_style_text_color(s_fs_title, lv_color_black(), 0);
    lv_label_set_long_mode(s_fs_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_fs_title, "Music Files");

    lv_obj_t *back_btn = lv_btn_create(s_fs_cont);
    lv_obj_set_pos(back_btn, FS_W - 36, 1);
    lv_obj_set_size(back_btn, 34, 16);
    lv_obj_set_style_radius(back_btn, 3, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, fs_back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0x0000FF), 0);
    lv_obj_center(back_lbl);

    s_fs_list = lv_list_create(s_fs_cont);
    lv_obj_set_pos(s_fs_list, 0, 22);
    /* 修改列表高度：原为 FS_H - 36，现减少高度以为大按钮腾出空间 */
    /* 假设底部导航区高度设为 40 (原18)，则列表高度 = FS_H(220) - 22(标题) - 40(底部) = 158 */
    lv_obj_set_size(s_fs_list, FS_W, FS_H - 58); 
    lv_obj_set_style_bg_color(s_fs_list, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_fs_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_fs_list, 0, 0);
    lv_obj_set_style_pad_all(s_fs_list, 0, 0);

    /* 修改导航区域 Y 坐标，向上移动 */
    int nav_y = FS_H - 35; 

    s_fs_prev_btn = lv_btn_create(s_fs_cont);
    lv_obj_set_pos(s_fs_prev_btn, 2, nav_y);
    /* 修改按钮大小：变大变高 (40x32) */
    lv_obj_set_size(s_fs_prev_btn, 40, 32); 
    lv_obj_set_style_radius(s_fs_prev_btn, 6, 0);
    /* 修改按钮颜色：变为浅灰色，更深 */
    lv_obj_set_style_bg_color(s_fs_prev_btn, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_fs_prev_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_fs_prev_btn, 0, 0);
    lv_obj_add_event_cb(s_fs_prev_btn, fs_prev_click_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *prev_lbl = lv_label_create(s_fs_prev_btn);
    lv_label_set_text(prev_lbl, "<");
    lv_obj_set_style_text_font(prev_lbl, &chinese_16, 0);
    /* 文字颜色也设为蓝色，更协调 */
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(0x0000FF), 0);
    lv_obj_center(prev_lbl);

    s_fs_page_lbl = lv_label_create(s_fs_cont);
    /* 调整页码标签位置，使其在新的空间内居中 */
    /* 按钮宽40，总宽135。间隙约 (135-40-40)/3 = 18.3 */
    lv_obj_set_pos(s_fs_page_lbl, 47, nav_y + 8); 
    lv_obj_set_size(s_fs_page_lbl, 40, 16); /* 稍微变窄以适应新布局 */
    lv_obj_set_style_text_align(s_fs_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_fs_page_lbl, &chinese_16, 0);
    lv_obj_set_style_text_color(s_fs_page_lbl, lv_color_black(), 0);
    lv_label_set_text(s_fs_page_lbl, "1/1");

    s_fs_next_btn = lv_btn_create(s_fs_cont);
    /* 调整 X 坐标，靠右对齐 */
    lv_obj_set_pos(s_fs_next_btn, FS_W - 42, nav_y); 
    /* 修改按钮大小：变大变高 (40x32) */
    lv_obj_set_size(s_fs_next_btn, 40, 32);
    lv_obj_set_style_radius(s_fs_next_btn, 6, 0);
    /* 修改按钮颜色：变为浅灰色，更深 */
    lv_obj_set_style_bg_color(s_fs_next_btn, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_fs_next_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_fs_next_btn, 0, 0);
    lv_obj_add_event_cb(s_fs_next_btn, fs_next_click_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *next_lbl = lv_label_create(s_fs_next_btn);
    lv_label_set_text(next_lbl, ">");
    lv_obj_set_style_text_font(next_lbl, &chinese_16, 0);
    /* 文字颜色也设为蓝色 */
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(0x0000FF), 0);
    lv_obj_center(next_lbl);

    s_fs_group = "sdcard";
    fs_browser_show_page(0);

    /* 打开后自动定位到主界面当前播放的歌曲 */
    fs_browser_jump_to_current();
}

/* 定位到当前播放歌曲: 进入所在目录、跳到所在页并高亮该文件 */
static void fs_browser_jump_to_current(void)
{
    if (!s_fs_overlay) return;

    const char *group = player_current_group();
    const char *name  = player_current_name();
    if (!group || !name || !g_fs_cache) return;

    /* 切换到当前播放歌曲所在的组 (目录) */
    if (strncmp(group, "sdcard_", 7) == 0) {
        s_fs_inside = true;
        lv_label_set_text(s_fs_title, group + 7);
    } else {
        s_fs_inside = false;
        lv_label_set_text(s_fs_title, "Music Files");
    }
    s_fs_group = group;

    /* 在组内 (含目录项, 与列表显示顺序一致) 找到该文件的位置 */
    int pos = -1;
    int seen = 0;
    for (int i = 0; i < g_fs_cache->count; i++) {
        fs_entry_t *e = &g_fs_cache->entries[i];
        if (strcmp(e->group, group) == 0) {
            if (strcmp(e->name, name) == 0) {
                pos = seen;
                break;
            }
            seen++;
        }
    }

    if (pos < 0) {
        fs_browser_show_page(0);
        return;
    }

    int page = pos / FS_ITEMS;
    fs_browser_show_page(page);

    /* 滚动到可见区域, 让高亮的当前歌曲行显示出来 */
    if (s_fs_current_btn) {
        lv_obj_scroll_to_view(s_fs_current_btn, LV_ANIM_OFF);
        s_fs_current_btn = NULL;
    }
}

/* 两向同步: 播放歌曲变化时刷新绿色高亮。
 * 仅当浏览器打开且当前歌曲在当前视图内 (本组 / 根目录高亮文件夹) 时重绘, 保留滚动位置。 */
void fs_browser_refresh(void)
{
    if (!s_fs_overlay || !g_fs_cache || !s_fs_group) return;

    const char *group = player_current_group();
    const char *name  = player_current_name();
    if (!group || !name) return;

    bool in_view = (strcmp(s_fs_group, group) == 0)
                   || (strcmp(s_fs_group, "sdcard") == 0
                       && strncmp(group, "sdcard_", 7) == 0);
    if (!in_view) return;

    lv_coord_t scroll_y = lv_obj_get_scroll_y(s_fs_list);
    fs_browser_show_page(s_fs_page);
    lv_obj_scroll_to_y(s_fs_list, scroll_y, LV_ANIM_OFF);
}

/* 导航到当前播放歌曲所在目录并高亮 (SD 恢复 / 打开浏览器时使用) */
void fs_browser_jump(void)
{
    if (!s_fs_overlay) return;
    fs_browser_jump_to_current();
}


void fs_menu_click_cb(lv_event_t *e)
{
    if (s_fs_overlay) {
        /* 已打开时再点一次: 关闭浏览器 */
        fs_browser_close();
    } else {
        /* 未打开: 打开浏览器 (打开时自动定位到当前播放歌曲) */
        fs_browser_open();
    }
}

void fs_browser_on_sd_ready(void)
{
    if (s_fs_overlay) {
        s_fs_group = "sdcard";
        s_fs_inside = false;
        lv_label_set_text(s_fs_title, "Music Files");
        fs_browser_show_page(0);
    }
}

void fs_browser_on_sd_remove(void)
{
    if (s_fs_overlay) {
        s_fs_group = NULL;
        fs_browser_show_page(0);
    }
}

/* ────────────────────────────────────────────────────────────
 * 蓝牙设备列表
 * ──────────────────────────────────────────────────────────── */
#define BT_W        135
#define BT_H        220
#define BT_X        13
#define BT_Y        3
#define BT_ROW_H    36
#define BT_BUF_MAX  16
#define COLOR_BT_BLUE  lv_color_hex(0x2196F3)
#define COLOR_BT_RED   lv_color_hex(0xE53935)

static lv_obj_t *s_bt_overlay   = NULL;
static lv_obj_t *s_bt_cont      = NULL;
static lv_obj_t *s_bt_title     = NULL;
static lv_obj_t *s_bt_list      = NULL;
static lv_obj_t *s_bt_card      = NULL;
static lv_obj_t *s_bt_card_name = NULL;
static lv_obj_t *s_bt_action_btn  = NULL;
static lv_obj_t *s_bt_action_lbl  = NULL;

static bool         s_bt_open = false;
static bool         s_bt_scanning = false;
static bt_state_t   s_bt_state = BT_STATE_DISCONNECTED;
static char         s_bt_connect_name[32];

static char         s_bt_display[BT_BUF_MAX][32];
static int          s_bt_display_count = 0;
static char         s_bt_pending[BT_BUF_MAX][32];
static int          s_bt_pending_count = 0;

static bt_a2dp_iface_t *s_bt_iface = NULL;

static void bt_send_cmd(bt_cmd_type_t type, const char *name)
{
    if (!s_bt_iface) return;
    bt_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    if (name) {
        strncpy(cmd.device_name, name, sizeof(cmd.device_name) - 1);
    }
    xQueueSend(s_bt_iface->cmd_queue, &cmd, 0);
}

static void bt_apply_state(bt_state_t state, const char *name);
static void bt_maybe_start_scan(void);

/* 仅在「列表打开 + 未连接 + 未在扫描」时发一次扫描 */
static void bt_maybe_start_scan(void)
{
    if (!s_bt_open || s_bt_state != BT_STATE_DISCONNECTED || s_bt_scanning) return;
    s_bt_scanning = true;
    bt_send_cmd(BT_CMD_SCAN, NULL);
}

static void bt_item_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *name = (const char *)lv_obj_get_user_data(btn);
    if (!name) return;

    s_bt_scanning = false;
    bt_apply_state(BT_STATE_CONNECTING, name);
    bt_send_cmd(BT_CMD_CONNECT, name);
}

static void bt_action_click_cb(lv_event_t *e)
{
    if (s_bt_state == BT_STATE_CONNECTED) {
        bt_send_cmd(BT_CMD_DISCONNECT, NULL);
    }
}

static void bt_show_list_view(void)
{
    lv_obj_clear_flag(s_bt_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bt_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bt_action_btn, LV_OBJ_FLAG_HIDDEN);
}

/* 依据给定状态更新影子状态 + (列表打开时)渲染对应视图 */
static void bt_apply_state(bt_state_t state, const char *name)
{
    s_bt_state = state;
    if (name && name[0]) {
        strncpy(s_bt_connect_name, name, sizeof(s_bt_connect_name) - 1);
        s_bt_connect_name[sizeof(s_bt_connect_name) - 1] = '\0';
    }

    if (!s_bt_open) return;

    s_bt_scanning = false;

    switch (state) {
    case BT_STATE_DISCONNECTED:
        bt_show_list_view();
        bt_maybe_start_scan();
        break;
    case BT_STATE_CONNECTING:
        lv_label_set_text(s_bt_card_name, s_bt_connect_name);
        lv_obj_set_style_bg_color(s_bt_action_btn, COLOR_BT_BLUE, 0);
        lv_label_set_text(s_bt_action_lbl, "Connecting");
        lv_obj_clear_flag(s_bt_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_bt_action_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bt_list, LV_OBJ_FLAG_HIDDEN);
        break;
    case BT_STATE_CONNECTED:
        lv_label_set_text(s_bt_card_name, s_bt_connect_name);
        lv_obj_set_style_bg_color(s_bt_action_btn, COLOR_BT_RED, 0);
        lv_label_set_text(s_bt_action_lbl, "Disconnect");
        lv_obj_clear_flag(s_bt_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_bt_action_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bt_list, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }
}

static void bt_refresh_list(void)
{
    lv_obj_clean(s_bt_list);

    if (s_bt_display_count == 0) {
        lv_obj_t *btn = lv_list_add_btn(s_bt_list, NULL, "No devices");
        lv_obj_set_height(btn, BT_ROW_H);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        return;
    }

    for (int i = 0; i < s_bt_display_count; i++) {
        lv_obj_t *btn = lv_list_add_btn(s_bt_list, NULL, s_bt_display[i]);
        lv_obj_set_height(btn, BT_ROW_H);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_pad_left(btn, 8, 0);

        lv_obj_t *label = lv_obj_get_child(btn, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

        lv_obj_set_user_data(btn, s_bt_display[i]);
        lv_obj_add_event_cb(btn, bt_item_click_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void bt_overlay_click_cb(lv_event_t *e)
{
    bt_menu_click_cb(e);
}

static void bt_list_open(void)
{
    if (s_bt_open) return;

    s_bt_open = true;
    s_bt_pending_count = 0;
    s_bt_display_count = 0;

    s_bt_overlay = lv_btn_create(lv_scr_act());
    lv_obj_set_size(s_bt_overlay, 172, 320);
    lv_obj_set_pos(s_bt_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_bt_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bt_overlay, 0, 0);
    lv_obj_set_style_shadow_width(s_bt_overlay, 0, 0);
    lv_obj_add_event_cb(s_bt_overlay, bt_overlay_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(s_bt_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_bt_cont = lv_obj_create(s_bt_overlay);
    lv_obj_set_pos(s_bt_cont, BT_X, BT_Y);
    lv_obj_set_size(s_bt_cont, BT_W, BT_H);
    lv_obj_set_style_bg_color(s_bt_cont, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_bt_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bt_cont, 6, 0);
    lv_obj_set_style_border_color(s_bt_cont, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_width(s_bt_cont, 1, 0);
    lv_obj_set_style_pad_all(s_bt_cont, 0, 0);
    lv_obj_clear_flag(s_bt_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_bt_title = lv_label_create(s_bt_cont);
    lv_obj_set_pos(s_bt_title, 6, 4);
    lv_obj_set_size(s_bt_title, BT_W - 12, 22);
    lv_obj_set_style_text_font(s_bt_title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_bt_title, lv_color_black(), 0);
    lv_label_set_long_mode(s_bt_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_bt_title, "Bluetooth");

    s_bt_list = lv_list_create(s_bt_cont);
    lv_obj_set_pos(s_bt_list, 0, 28);
    lv_obj_set_size(s_bt_list, BT_W, BT_H - 28);
    lv_obj_set_style_bg_color(s_bt_list, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_bt_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bt_list, 0, 0);
    lv_obj_set_style_pad_all(s_bt_list, 0, 0);

    s_bt_card = lv_obj_create(s_bt_cont);
    lv_obj_set_pos(s_bt_card, 12, 40);
    lv_obj_set_size(s_bt_card, BT_W - 24, 72);
    lv_obj_set_style_bg_color(s_bt_card, COLOR_BT_BLUE, 0);
    lv_obj_set_style_bg_opa(s_bt_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bt_card, 12, 0);
    lv_obj_set_style_border_width(s_bt_card, 0, 0);
    lv_obj_set_style_pad_all(s_bt_card, 8, 0);
    lv_obj_clear_flag(s_bt_card, LV_OBJ_FLAG_SCROLLABLE);

    s_bt_card_name = lv_label_create(s_bt_card);
    lv_obj_set_style_text_font(s_bt_card_name, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_bt_card_name, lv_color_white(), 0);
    lv_label_set_long_mode(s_bt_card_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_bt_card_name, BT_W - 40);
    lv_label_set_text(s_bt_card_name, "");
    lv_obj_align(s_bt_card_name, LV_ALIGN_CENTER, 0, 0);

    s_bt_action_btn = lv_btn_create(s_bt_cont);
    lv_obj_set_pos(s_bt_action_btn, 12, BT_H - 48);
    lv_obj_set_size(s_bt_action_btn, BT_W - 24, 36);
    lv_obj_set_style_radius(s_bt_action_btn, 10, 0);
    lv_obj_set_style_bg_color(s_bt_action_btn, COLOR_BT_BLUE, 0);
    lv_obj_set_style_bg_opa(s_bt_action_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bt_action_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_bt_action_btn, 0, 0);
    lv_obj_add_event_cb(s_bt_action_btn, bt_action_click_cb, LV_EVENT_CLICKED, NULL);

    s_bt_action_lbl = lv_label_create(s_bt_action_btn);
    lv_label_set_text(s_bt_action_lbl, "Connecting");
    lv_obj_set_style_text_font(s_bt_action_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_bt_action_lbl, lv_color_white(), 0);
    lv_obj_center(s_bt_action_lbl);

    lv_obj_add_flag(s_bt_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bt_action_btn, LV_OBJ_FLAG_HIDDEN);

    bt_apply_state(s_bt_state, NULL);
    bt_send_cmd(BT_CMD_GET_STATE, NULL);
}

static void bt_list_close(void)
{
    if (!s_bt_open) return;

    if (s_bt_scanning) {
        bt_send_cmd(BT_CMD_STOP_SCAN, NULL);
    }

    s_bt_scanning = false;
    s_bt_open = false;

    if (s_bt_overlay) {
        lv_obj_del(s_bt_overlay);
    }
    s_bt_overlay   = NULL;
    s_bt_cont      = NULL;
    s_bt_title     = NULL;
    s_bt_list      = NULL;
    s_bt_card      = NULL;
    s_bt_card_name = NULL;
    s_bt_action_btn  = NULL;
    s_bt_action_lbl  = NULL;
}

void bt_list_init(bt_a2dp_iface_t *iface)
{
    s_bt_iface = iface;
}

void bt_menu_click_cb(lv_event_t *e)
{
    if (s_bt_open) {
        bt_list_close();
    } else {
        bt_list_open();
    }
}

void bt_list_on_device_found(const char *name)
{
    if (!s_bt_open) return;
    if (s_bt_pending_count >= BT_BUF_MAX) return;
    strncpy(s_bt_pending[s_bt_pending_count], name, sizeof(s_bt_pending[0]) - 1);
    s_bt_pending[s_bt_pending_count][sizeof(s_bt_pending[0]) - 1] = '\0';
    s_bt_pending_count++;
}

void bt_list_on_scan_done(void)
{
    if (!s_bt_open) return;
    s_bt_scanning = false;

    s_bt_display_count = s_bt_pending_count;
    for (int i = 0; i < s_bt_pending_count; i++) {
        strncpy(s_bt_display[i], s_bt_pending[i], sizeof(s_bt_display[0]) - 1);
        s_bt_display[i][sizeof(s_bt_display[0]) - 1] = '\0';
    }
    s_bt_pending_count = 0;

    bt_refresh_list();

    bt_maybe_start_scan();
}

void bt_list_on_connected(const char *name)
{
    bt_apply_state(BT_STATE_CONNECTED, name);
}

void bt_list_on_connect_failed(const char *name)
{
    bt_apply_state(BT_STATE_DISCONNECTED, NULL);
}

void bt_list_on_disconnected(void)
{
    memset(s_bt_connect_name, 0, sizeof(s_bt_connect_name));
    bt_apply_state(BT_STATE_DISCONNECTED, NULL);
}

void bt_list_on_state_rsp(bt_state_t state, const char *name)
{
    bt_apply_state(state, name);
}

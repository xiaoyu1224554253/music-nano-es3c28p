#include <string.h>
#include <stdio.h>
#include "lvgl.h"
#include "sys_monitor.h"
#include "ui_res.h"
#include "ui_player.h"
#include "menu.h"

extern const lv_font_t lv_font_global_16;

#define FS_W        135    /* 面板宽 */
#define FS_H        220    /* 面板高 */
#define FS_X        0      /* 面板 X (从左上角展开) */
#define FS_Y        3      /* 面板 Y */
#define FS_ITEMS    30     /* 每页条目数 */
#define FS_ROW_H    18     /* 行高 (实际运行时覆盖为 30) */

/* 文件浏览器控件句柄 */
static lv_obj_t  *s_fs_overlay   = NULL;  /* 全屏透明遮罩 */
static lv_obj_t  *s_fs_cont      = NULL;  /* 白色面板容器 */
static lv_obj_t  *s_fs_title     = NULL;  /* 标题 */
static lv_obj_t  *s_fs_list      = NULL;  /* 文件列表 */
static lv_obj_t  *s_fs_page_lbl  = NULL;  /* 页码标签 */
static lv_obj_t  *s_fs_prev_btn  = NULL;  /* 上一页按钮 */
static lv_obj_t  *s_fs_next_btn  = NULL;  /* 下一页按钮 */

static int   s_fs_page            = 0;     /* 当前页 */
static int   s_fs_total           = 0;     /* 总页数 */
static bool  s_fs_inside          = false; /* 是否已进入子目录 */
static const char *s_fs_group    = NULL;   /* 当前分组 (sdcard / sdcard%a%b) */
static lv_obj_t *s_fs_current_btn = NULL; /* 当前播放歌曲对应的列表行按钮 */

static lv_img_dsc_t s_fs_icon_dir;    /* 文件夹图标 */
static lv_img_dsc_t s_fs_icon_music;  /* 音乐文件图标 */

static void (*s_play_cb)(const char *group, const char *name) = NULL;   /* 点击播放回调 */

static void fs_browser_open(void);
static void fs_browser_close(void);
static void fs_browser_show_page(int page);
static void fs_browser_enter_dir(const char *parent_group, const char *name);
static void fs_browser_go_back(void);
static void fs_browser_jump_to_current(void);

/* 当前分组缓冲: s_fs_group 始终指向它, 便于拼接/回退 */
static char s_group_buf[FS_GROUP_MAX];

/* 设置当前分组 (拷贝进 s_group_buf); g=NULL 表示无分组 */
static void fs_set_group(const char *g)
{
    if (!g) {
        s_fs_group = NULL;
        return;
    }
    size_t n = strnlen(g, sizeof(s_group_buf) - 1);
    memcpy(s_group_buf, g, n);
    s_group_buf[n] = '\0';
    s_fs_group = s_group_buf;
}

/* 由父 group + 子目录名拼出子 group: parent%name */
static void fs_child_group(const char *parent, const char *name,
                           char *out, size_t out_size)
{
    snprintf(out, out_size, "%s%%%s", parent, name);
}

/* group 的显示名 (最后一段); 根或无返回 "Music Files" */
static const char *fs_group_display_name(const char *group)
{
    if (!group) return "Music Files";
    const char *last = strrchr(group, '%');
    return last ? last + 1 : "Music Files";
}

/* 注册点击播放回调: cb=播放函数 */
void fs_list_set_play_cb(void (*cb)(const char *group, const char *name))
{
    s_play_cb = cb;
}

/* 统计指定分组的条目数 */
static int fs_cache_count_for_group(const char *group)
{
    if (!g_fs_cache || !group) return 0;
    int count = 0;
    for (int i = 0; i < g_fs_cache->count; i++) {
        if (strcmp(g_fs_cache->entries[i].group, group) == 0) count++;
    }
    return count;
}

/* 取分组内第 idx 个缓存条目 */
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

/* 列表项点击: 目录→进入, 文件→播放 */
static void fs_item_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    fs_entry_t *entry = (fs_entry_t *)lv_obj_get_user_data(btn);   /* 条目存 user_data */
    if (!entry) return;

    if (entry->is_dir) {
        fs_browser_enter_dir(entry->group, entry->name);   /* 进入子目录 */
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
    if (!group || !name || !entry || !s_fs_group) return false;

    /* 文件条目: 组与文件名都匹配 */
    if (!entry->is_dir) {
        return strcmp(s_fs_group, group) == 0 && strcmp(entry->name, name) == 0;
    }

    /* 目录条目: 当前播放歌曲所在目录是其子孙 → 高亮该文件夹 */
    char child[FS_GROUP_MAX];
    fs_child_group(entry->group, entry->name, child, sizeof(child));
    size_t cl = strlen(child);
    return strncmp(group, child, cl) == 0
           && (group[cl] == '\0' || group[cl] == '%');
}

/* 向列表添加一个条目 (带图标), 若为当前播放歌曲则绿色高亮 */
static void fs_add_item(fs_entry_t *entry)
{
    lv_img_dsc_t *icon = entry->is_dir ? &s_fs_icon_dir : &s_fs_icon_music;

    lv_obj_t *btn = lv_list_add_btn(s_fs_list, icon, entry->name);
    lv_obj_set_height(btn, 30);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_pad_top(btn, 3, 0);

    lv_obj_t *label = lv_obj_get_child(btn, 1);
    lv_obj_set_style_text_font(label, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_height(label, lv_font_get_line_height(&lv_font_global_16));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    lv_obj_set_user_data(btn, entry);   /* 存条目指针供点击回调 */
    lv_obj_add_event_cb(btn, fs_item_click_cb, LV_EVENT_CLICKED, NULL);

    /* 高亮当前正在播放的歌曲行 */
    if (fs_entry_is_current(entry)) {
        s_fs_current_btn = btn;
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xB7F7C2), 0);   /* 淡绿高亮 */
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    }
}

/* 显示指定页: 计算页数/页码, 重建列表行 */
static void fs_browser_show_page(int page)
{
    s_fs_page = page;
    s_fs_current_btn = NULL;

    int total = fs_cache_count_for_group(s_fs_group);
    s_fs_total = (total + FS_ITEMS - 1) / FS_ITEMS;   /* 总页数 (向上取整) */
    if (s_fs_total < 1) s_fs_total = 1;

    int start = page * FS_ITEMS;   /* 本页首条目 */
    int end   = (start + FS_ITEMS) < total ? (start + FS_ITEMS) : total;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d", page + 1, s_fs_total);   /* 页码 */
    lv_label_set_text(s_fs_page_lbl, buf);

    /* 首页/末页禁用对应翻页按钮 */
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

    lv_obj_clean(s_fs_list);   /* 清空重建 */

    if (total == 0) {   /* 空目录提示 */
        lv_obj_t *btn = lv_list_add_btn(s_fs_list, NULL, "无音乐文件");
        lv_obj_set_height(btn, 30);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_pad_top(btn, 3, 0);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        lv_obj_set_style_text_font(label, &lv_font_global_16, 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        return;
    }

    for (int i = start; i < end; i++) {   /* 填充本页条目 */
        fs_entry_t *entry = fs_cache_entry_for_group(s_fs_group, i);
        if (entry) fs_add_item(entry);
    }
}

/* 上一页 */
static void fs_prev_click_cb(lv_event_t *e)
{
    if (s_fs_page > 0) {
        fs_browser_show_page(s_fs_page - 1);
    }
}

/* 下一页 */
static void fs_next_click_cb(lv_event_t *e)
{
    if (s_fs_page < s_fs_total - 1) {
        fs_browser_show_page(s_fs_page + 1);
    }
}

/* 实际关闭浏览器: 删除遮罩 (连带删除子控件) 并复位状态 */
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
        static const panel_anim_cfg_t cfg = {
            .overlay_ref = &s_fs_overlay,
            .cont_ref    = &s_fs_cont,
            .w = FS_W, .h = FS_H,
            .x = ANIM_FS_X, .y = ANIM_FS_Y,
            .anchor_right = false,
            .open_real  = fs_browser_open,
            .close_real = fs_browser_close,
        };
        panel_anim_close(&cfg);
}

static void fs_browser_enter_dir(const char *parent_group, const char *name)
{
    s_fs_inside = true;

    char child[FS_GROUP_MAX];
    fs_child_group(parent_group, name, child, sizeof(child));   /* 组名 = 父%子 */
    fs_set_group(child);

    lv_label_set_text(s_fs_title, name);   /* 标题 = 目录名 */

    fs_browser_show_page(0);
}

/* 返回上一级目录 (根则不变) */
static void fs_browser_go_back(void)
{
    if (!s_fs_group) return;

    /* 截到最后一个 '%' 得到父 group */
    char parent[FS_GROUP_MAX];
    size_t n = strnlen(s_fs_group, sizeof(parent) - 1);
    memcpy(parent, s_fs_group, n);
    parent[n] = '\0';

    char *last = strrchr(parent, '%');
    if (last) {
        *last = '\0';
        fs_set_group(parent);
    } else {
        fs_set_group("sdcard");   /* 已在根 */
    }

    s_fs_inside = (strcmp(s_fs_group, "sdcard") != 0);
    lv_label_set_text(s_fs_title, fs_group_display_name(s_fs_group));
    fs_browser_show_page(0);
}

/* 返回按钮 */
static void fs_back_click_cb(lv_event_t *e)
{
    fs_browser_go_back();
}

/* 实际打开浏览器: 创建控件 + 加载根目录列表 */
static void fs_browser_open(void)
{
    if (s_fs_overlay) {
        fs_browser_close();
    }
    /* 加载文件夹/音乐图标 */
    s_fs_icon_dir.header.w = 16;
    s_fs_icon_dir.header.h = 21;
    s_fs_icon_dir.data_size = 16 * 21 * 2;
    s_fs_icon_dir.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_fs_icon_dir.data = (const uint8_t *)icon_file[0];
    s_fs_icon_music.header.w = 16;
    s_fs_icon_music.header.h = 21;
    s_fs_icon_music.data_size = 16 * 21 * 2;
    s_fs_icon_music.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_fs_icon_music.data = (const uint8_t *)icon_file[1];

    /* 全屏透明遮罩 */
    s_fs_overlay = lv_btn_create(lv_scr_act());
    lv_obj_set_size(s_fs_overlay, 172, 320);
    lv_obj_set_pos(s_fs_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_fs_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_fs_overlay, 0, 0);
    lv_obj_set_style_shadow_width(s_fs_overlay, 0, 0);
    lv_obj_add_event_cb(s_fs_overlay, fs_overlay_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(s_fs_overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* 白色面板容器 */
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

    /* 标题 */
    s_fs_title = lv_label_create(s_fs_cont);
    lv_obj_set_pos(s_fs_title, 4, 0);
    lv_obj_set_size(s_fs_title, FS_W - 40, 22);
    lv_obj_set_style_text_font(s_fs_title, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(s_fs_title, lv_color_black(), 0);
    lv_label_set_long_mode(s_fs_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_fs_title, "Music Files");

    /* 返回(上一级)按钮 */
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

    /* 文件列表 */
    s_fs_list = lv_list_create(s_fs_cont);
    lv_obj_set_pos(s_fs_list, 0, 22);
    /* 修改列表高度：原为 FS_H - 36，现减少高度以为大按钮腾出空间 */
    /* 假设底部导航区高度设为 40 (原18)，则列表高度 = FS_H(220) - 22(标题) - 40(底部) = 158 */
    lv_obj_set_size(s_fs_list, FS_W, FS_H - 58);
    lv_obj_set_style_bg_color(s_fs_list, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_fs_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_fs_list, 0, 0);
    lv_obj_set_style_pad_all(s_fs_list, 0, 0);

    /* 底部导航区 Y */
    int nav_y = FS_H - 35;

    /* 上一页按钮 */
    s_fs_prev_btn = lv_btn_create(s_fs_cont);
    lv_obj_set_pos(s_fs_prev_btn, 2, nav_y);
    lv_obj_set_size(s_fs_prev_btn, 40, 32);
    lv_obj_set_style_radius(s_fs_prev_btn, 6, 0);
    lv_obj_set_style_bg_color(s_fs_prev_btn, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_fs_prev_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_fs_prev_btn, 0, 0);
    lv_obj_add_event_cb(s_fs_prev_btn, fs_prev_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *prev_lbl = lv_label_create(s_fs_prev_btn);
    lv_label_set_text(prev_lbl, "<");
    lv_obj_set_style_text_font(prev_lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(prev_lbl, lv_color_hex(0x0000FF), 0);
    lv_obj_center(prev_lbl);

    /* 页码标签 */
    s_fs_page_lbl = lv_label_create(s_fs_cont);
    lv_obj_set_pos(s_fs_page_lbl, 47, nav_y + 8);
    lv_obj_set_size(s_fs_page_lbl, 40, 16);
    lv_obj_set_style_text_align(s_fs_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_fs_page_lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(s_fs_page_lbl, lv_color_black(), 0);
    lv_label_set_text(s_fs_page_lbl, "1/1");

    /* 下一页按钮 */
    s_fs_next_btn = lv_btn_create(s_fs_cont);
    lv_obj_set_pos(s_fs_next_btn, FS_W - 42, nav_y);
    lv_obj_set_size(s_fs_next_btn, 40, 32);
    lv_obj_set_style_radius(s_fs_next_btn, 6, 0);
    lv_obj_set_style_bg_color(s_fs_next_btn, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_opa(s_fs_next_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_fs_next_btn, 0, 0);
    lv_obj_add_event_cb(s_fs_next_btn, fs_next_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next_lbl = lv_label_create(s_fs_next_btn);
    lv_label_set_text(next_lbl, ">");
    lv_obj_set_style_text_font(next_lbl, &lv_font_global_16, 0);
    lv_obj_set_style_text_color(next_lbl, lv_color_hex(0x0000FF), 0);
    lv_obj_center(next_lbl);

    fs_set_group("sdcard");   /* 默认根目录 */
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
    fs_set_group(group);
    s_fs_inside = (strcmp(group, "sdcard") != 0);
    lv_label_set_text(s_fs_title, fs_group_display_name(group));

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
 * 仅当浏览器打开且当前分组是播放歌曲所在目录的祖先 (或相等) 时重绘, 保留滚动位置。 */
void fs_browser_refresh(void)
{
    if (!s_fs_overlay || !g_fs_cache || !s_fs_group) return;

    const char *group = player_current_group();
    const char *name  = player_current_name();
    if (!group || !name) return;

    size_t sl = strlen(s_fs_group);
    bool in_view = (strcmp(s_fs_group, group) == 0)
                   || (strncmp(group, s_fs_group, sl) == 0 && group[sl] == '%');
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
        /* 未打开: 打开浏览器 (带展开动画) */
        static const panel_anim_cfg_t cfg = {
            .overlay_ref = &s_fs_overlay,
            .cont_ref    = &s_fs_cont,
            .w = FS_W, .h = FS_H,
            .x = ANIM_FS_X, .y = ANIM_FS_Y,
            .anchor_right = false,
            .open_real  = fs_browser_open,
            .close_real = fs_browser_close,
        };
        panel_anim_open(&cfg);
    }
}

void fs_browser_on_sd_ready(void)
{
    if (s_fs_overlay) {
        fs_set_group("sdcard");
        s_fs_inside = false;
        lv_label_set_text(s_fs_title, "Music Files");
        fs_browser_show_page(0);
    }
}

void fs_browser_on_sd_remove(void)
{
    if (s_fs_overlay) {
        fs_set_group(NULL);
        fs_browser_show_page(0);
    }
}

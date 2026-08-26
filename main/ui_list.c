#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
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
static void bt_list_open(void);
static void bt_list_close(void);

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

/* ────────────────────────────────────────────
 * 通用面板 打开/关闭 展开动画 (截图到 PSRAM + 顶部图片层)
 * 文件浏览器与蓝牙列表共用
 * ──────────────────────────────────────────── */
#define ANIM_FS_START    30
#define ANIM_FS_MS_OPEN  150   /* 打开动画: 先快后慢 (ease-out) */
#define ANIM_FS_MS_CLOSE 100   /* 关闭动画: 线性 */
#define ANIM_PANEL_RADIUS 5    /* 面板圆角半径 (两容器统一写死) */

/* 动画图实际显示位置: 与真实面板渲染位置对齐 (实测含边框偏移) */
#define ANIM_FS_X      13
#define ANIM_FS_Y      11
#define ANIM_BT_X      26   /* BT_X=13 + 偏移13 (实测微调) */
#define ANIM_BT_Y      11   /* BT_Y=3  + 偏移8  */

typedef struct {
    lv_obj_t **overlay_ref;   /* 指向 overlay 指针 (全屏透明按钮) */
    lv_obj_t **cont_ref;      /* 指向 cont 指针 (白色面板容器) */
    int         w, h;         /* 面板尺寸 */
    int         x, y;         /* 动画图显示位置 */
    bool        anchor_right; /* true=从右上角展开 (蓝牙), false=从左上角展开 (文件) */
    void      (*open_real)(void);   /* 真实打开面板 */
    void      (*close_real)(void);  /* 真实关闭面板 */
} panel_anim_cfg_t;

static const panel_anim_cfg_t *s_anim_cfg    = NULL;
static lv_obj_t      *s_anim_bg_img  = NULL;   /* 顶部背景图 (主界面截图) */
static lv_obj_t      *s_anim_fs_img  = NULL;   /* 面板展开图 */
static lv_img_dsc_t   s_anim_dsc_main;         /* 主界面全屏截图 */
static lv_img_dsc_t   s_anim_dsc_fs;           /* 面板截图 */
static lv_img_dsc_t   s_anim_dsc_buf;          /* 合成缓冲 (背景+面板裁切) */
static uint8_t       *s_anim_buf_main = NULL;  /* PSRAM */
static uint8_t       *s_anim_buf_fs   = NULL;  /* PSRAM */
static uint8_t       *s_anim_buf_anim = NULL;  /* PSRAM 合成缓冲 */
static int32_t        s_anim_open     = 1;     /* 1=打开动画 0=关闭动画 */
static int64_t        s_anim_t0       = 0;     /* 动画起始 esp_timer us */
static lv_timer_t    *s_anim_timer    = NULL;  /* 动画驱动定时器 */

volatile bool g_anim_active = false;           /* 动画进行中 (供 lvgl_task 测量) */
static volatile int64_t s_anim_compose_us = 0; /* 最近一帧 compose 耗时 */
static volatile int64_t s_anim_refr_us    = 0; /* 最近一帧 LVGL 刷屏耗时 */
static int64_t  s_prev_frame_us = 0;           /* 上一帧时间戳 (瞬时 fps) */
static bool     s_prev_frame_ok = false;       /* 首帧无前一帧, fps 打印 0 */
static int s_anim_prev_w = 0, s_anim_prev_h = 0; /* 上一帧显示区域尺寸 (增量复用) */
static bool s_anim_first_frame = true;          /* 首帧需全量填充 (buf 未初始化) */
/* 本次 compose 的变化区域 (buf 坐标, 分两块 A=垂直列条 B=水平行条, 不合并避免推方块) */
static lv_area_t s_anim_chg_a;
static lv_area_t s_anim_chg_b;
static bool      s_anim_chg_valid = false;

static void *fs_anim_alloc(uint32_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    return p;
}

/* 单次合成缓冲:
 * 动画面板区域 = 显示区域 (w,h)。y>=h 的行整行取背景;
 * y<h 的行内, 显示区域圆角内取容器像素, 圆角外取背景。
 * 每个目标像素只写一次 (原为两次复制: 整块背景 + 容器覆盖)。
 * anchor_right 时容器贴右 (蓝牙从右上展开), 否则贴左 (文件浏览器)。 */
static void fs_anim_compose(int w, int h)
{
    int ptw = s_anim_cfg->w, pth = s_anim_cfg->h;
    int r = ANIM_PANEL_RADIUS;
    int ox = s_anim_cfg->anchor_right ? (ptw - w) : 0;  /* 容器区在目标缓冲的 x 偏移 */

    for (int y = 0; y < pth; y++) {
        uint8_t *dst = s_anim_buf_anim + y * ptw * 2;
        uint8_t *bg  = s_anim_buf_main + ((s_anim_cfg->y + y) * 172 + s_anim_cfg->x) * 2;

        /* y >= h: 容器未覆盖, 整行背景 */
        if (y >= h) {
            memcpy(dst, bg, ptw * 2);
            continue;
        }

        /* 该行显示区域圆角内 x 区间 (基于当前显示区域宽 w, 圆心在四角 (r,r)) */
        int rx0 = 0, rx1 = w - 1;
        if (y < r) {
            /* 顶角行: 圆心 y=r, 该行距圆心 dy = r-y */
            int dy = r - y;
            if (dy > 0) {
                lv_sqrt_res_t q;
                lv_sqrt(r * r - dy * dy, &q, 0x8000);
                int s = q.i;
                rx0 = r - s; rx1 = w - 1 - (r - s);
            }
        } else if (y >= h - r) {
            /* 底角行: 圆心 y = h-1-r, 该行距圆心 dy = y-(h-1-r) */
            int dy = y - (h - 1 - r);
            if (dy > 0) {
                lv_sqrt_res_t q;
                lv_sqrt(r * r - dy * dy, &q, 0x8000);
                int s = q.i;
                rx0 = r - s; rx1 = w - 1 - (r - s);
            }
        }
        if (rx0 < 0) rx0 = 0;
        if (rx1 > w - 1) rx1 = w - 1;
        if (rx0 > rx1) rx0 = rx1;

        /* 容器区圆角内区间在目标缓冲的 x 范围 (目标 ox 与源偏移一致, 直接对齐) */
        int cx0 = ox + rx0, cx1 = ox + rx1;

        /* 复制: 圆角内取容器, 圆角外取背景, 逐段 memcpy */
        int pos = 0;
        while (pos < ptw) {
            if (pos < cx0) {
                memcpy(dst + pos * 2, bg + pos * 2, (cx0 - pos) * 2);
                pos = cx0;
            } else if (pos <= cx1) {
                memcpy(dst + pos * 2, s_anim_buf_fs + y * ptw * 2 + pos * 2,
                       (cx1 - pos + 1) * 2);
                pos = cx1 + 1;
            } else {
                memcpy(dst + pos * 2, bg + pos * 2, (ptw - pos) * 2);
                pos = ptw;
            }
        }
    }
}

/* 单行/单区段复制: 把目标缓冲 [tx0,tx1) 区间按当前帧 (w,h) 的圆角规则填好。
 * 打开动画时复用旧帧已填充区域, 只重写新增的右侧条/底部条 (背景不用重新复制);
 * 关闭动画时把回收条改回背景。 */
static void fs_anim_fill_seg(int y, int tx0, int tx1, int w, int h)
{
    if (tx0 >= tx1) return;
    int ptw = s_anim_cfg->w;
    int r = ANIM_PANEL_RADIUS;
    int ox = s_anim_cfg->anchor_right ? (ptw - w) : 0;  /* 容器区在目标缓冲的 x 偏移 */

    uint8_t *dst = s_anim_buf_anim + y * ptw * 2;
    uint8_t *bg  = s_anim_buf_main + ((s_anim_cfg->y + y) * 172 + s_anim_cfg->x) * 2;
    uint8_t *fs  = s_anim_buf_fs + y * ptw * 2;

    /* y >= h: 容器未覆盖, 整段背景 */
    if (y >= h) {
        memcpy(dst + tx0 * 2, bg + tx0 * 2, (tx1 - tx0) * 2);
        return;
    }

    /* 该行显示区域圆角内 x 区间 (窗口坐标 [0,w)) */
    int rx0 = 0, rx1 = w - 1;
    if (y < r) {
        int dy = r - y;
        if (dy > 0) {
            lv_sqrt_res_t q;
            lv_sqrt(r * r - dy * dy, &q, 0x8000);
            int s = q.i;
            rx0 = r - s; rx1 = w - 1 - (r - s);
        }
    } else if (y >= h - r) {
        int dy = y - (h - 1 - r);
        if (dy > 0) {
            lv_sqrt_res_t q;
            lv_sqrt(r * r - dy * dy, &q, 0x8000);
            int s = q.i;
            rx0 = r - s; rx1 = w - 1 - (r - s);
        }
    }
    if (rx0 < 0) rx0 = 0;
    if (rx1 > w - 1) rx1 = w - 1;
    if (rx0 > rx1) rx0 = rx1;

    /* 圆角区间 -> 目标缓冲 x 范围 */
    int cx0 = ox + rx0, cx1 = ox + rx1;

    /* 复制 [tx0,tx1): 圆角内取容器, 圆角外取背景 */
    int pos = tx0;
    while (pos < tx1) {
        if (pos < cx0) {
            int e = (tx1 < cx0) ? tx1 : cx0;
            memcpy(dst + pos * 2, bg + pos * 2, (e - pos) * 2);
            pos = e;
        } else if (pos <= cx1) {
            int e = (tx1 < cx1 + 1) ? tx1 : cx1 + 1;
            memcpy(dst + pos * 2, fs + pos * 2, (e - pos) * 2);
            pos = e;
        } else {
            memcpy(dst + pos * 2, bg + pos * 2, (tx1 - pos) * 2);
            pos = tx1;
        }
    }
}

/* 增量合成: 相比上一帧 (s_anim_prev_w,h), 只重写变化区域, 复用旧帧像素。
 * 原理: 展开/收缩是"裁切窗口"而非整图缩放, 只有新增条/回收条变化。
 * - 中间行 (无圆角): 只写新增的列条, 其余像素旧帧已正确。
 * - 顶/底圆角行 (各 r 行): 圆角位置随 w/h 移动, 整行重写最省心。
 * anchor_right 时窗口坐标与目标缓冲 x 有偏移, 由 fs_anim_fill_seg 处理。 */
/* 把 buf 坐标变化区域映射到屏幕坐标并 invalidate (不推全屏, 只推变化矩形) */
static void fs_anim_inv_area(const lv_area_t *buf_area)
{
    lv_area_t scr;
    lv_area_set(&scr,
                s_anim_cfg->x + buf_area->x1,
                s_anim_cfg->y + buf_area->y1,
                s_anim_cfg->x + buf_area->x2,
                s_anim_cfg->y + buf_area->y2);
    lv_obj_invalidate_area(s_anim_fs_img, &scr);
}

static void fs_anim_compose_incr(int w, int h)
{
    int ptw = s_anim_cfg->w;
    int r = ANIM_PANEL_RADIUS;
    int w_prev = s_anim_prev_w, h_prev = s_anim_prev_h;
    int ox = s_anim_cfg->anchor_right ? (ptw - w) : 0;
    bool ar = s_anim_cfg->anchor_right;

    /* 计算变化区域 (buf 坐标, 分两块矩形 A=列条 B=水平条, 不合并避免 L 型变方块) */
    if (s_anim_open) {
        /* ── 打开: 新增列条 + 新增底部条 ── */
        /* 块 A (列条): anchor_left 左扩 r 覆盖顶部右上角圆角移动, anchor_right 右缘固定 */
        if (ar) {
            lv_area_set(&s_anim_chg_a, ptw - w, 0, ptw - w_prev - 1, h - 1);
        } else {
            int x0 = w_prev - r; if (x0 < 0) x0 = 0;
            lv_area_set(&s_anim_chg_a, x0, 0, w - 1, h - 1);
        }
        /* 块 B (底部条): 旧底角行变中间行 + 新增底部, 起点 h_prev-r 覆盖角落 */
        int by = (h_prev - r > 0) ? (h_prev - r) : 0;
        if (ar) {
            lv_area_set(&s_anim_chg_b, ptw - w_prev, by, ptw - 1, h - 1);
        } else {
            lv_area_set(&s_anim_chg_b, 0, by, w_prev - 1, h - 1);
        }
    } else {
        /* ── 关闭: 回收列条 + 回收底部条 ── */
        if (ar) {
            lv_area_set(&s_anim_chg_a, ptw - w_prev, 0, ptw - w - 1, h - 1);
        } else {
            int x0 = w - r; if (x0 < 0) x0 = 0;
            lv_area_set(&s_anim_chg_a, x0, 0, w_prev - 1, h - 1);
        }
        int by = (h - r > 0) ? (h - r) : 0;
        if (ar) {
            lv_area_set(&s_anim_chg_b, ptw - w_prev, by, ptw - 1, h_prev - 1);
        } else {
            lv_area_set(&s_anim_chg_b, 0, by, w_prev - 1, h_prev - 1);
        }
    }
    s_anim_chg_valid = true;

    if (s_anim_open) {
        /* ── 打开: 区域扩张 ── */
        int new_w = w - w_prev;

        /* 1. 新增列条 (仅中间行): anchor_left 在 [w_prev,w), anchor_right 在 [ptw-w,ptw-w_prev) */
        if (new_w > 0) {
            int sx0 = s_anim_cfg->anchor_right ? (ptw - w) : w_prev;
            int sx1 = sx0 + new_w;
            int y_mid_lo = r;
            int y_mid_hi = (h_prev - r > 0) ? (h_prev - r) : 0;   /* 两帧都是中间行 */
            for (int y = y_mid_lo; y < y_mid_hi; y++) {
                fs_anim_fill_seg(y, sx0, sx1, w, h);
            }
        }
        /* 2. 顶部圆角行整行重写 */
        for (int y = 0; y < r && y < h; y++) {
            fs_anim_fill_seg(y, ox, ox + w, w, h);
        }
        /* 3. 旧底角行 + 新增底部条: [h_prev-r, h) 整行重写 */
        int b0 = (h_prev - r > 0) ? (h_prev - r) : 0;
        for (int y = b0; y < h; y++) {
            fs_anim_fill_seg(y, ox, ox + w, w, h);
        }
    } else {
        /* ── 关闭: 区域收缩 ── */
        int shrink_w = w_prev - w;
        int shrink_h = h_prev - h;

        /* 1. 回收右/左列条 (仅中间行) 改回背景 */
        if (shrink_w > 0) {
            int rx0 = s_anim_cfg->anchor_right ? (ptw - w_prev) : w;
            int rx1 = rx0 + shrink_w;
            int y_mid_lo = r;
            int y_mid_hi = (h - r > 0) ? (h - r) : 0;   /* 两帧都是中间行 */
            for (int y = y_mid_lo; y < y_mid_hi; y++) {
                fs_anim_fill_seg(y, rx0, rx1, w, h);
            }
        }
        /* 2. 回收底部条: [h, h_prev) 整行背景 */
        if (shrink_h > 0) {
            int wrow = s_anim_cfg->anchor_right ? ptw : w_prev;
            for (int y = h; y < h_prev; y++) {
                fs_anim_fill_seg(y, s_anim_cfg->anchor_right ? (ptw - w_prev) : 0,
                                 wrow, w, h);
            }
        }
        /* 3. 顶部圆角行整行重写 (覆盖到旧容器区右缘, 否则 [w,w_prev) 遗留旧像素) */
        {
            int x0 = s_anim_cfg->anchor_right ? (ptw - w_prev) : 0;
            int x1 = x0 + w_prev;
            for (int y = 0; y < r && y < h; y++) {
                fs_anim_fill_seg(y, x0, x1, w, h);
            }
        }
        /* 4. 底角行整行重写 (圆角随 h 上移, 同样覆盖到旧容器区右缘) */
        {
            int b0 = (h - r > 0) ? (h - r) : 0;
            int x0 = s_anim_cfg->anchor_right ? (ptw - w_prev) : 0;
            int x1 = x0 + w_prev;
            for (int y = b0; y < h; y++) {
                fs_anim_fill_seg(y, x0, x1, w, h);
            }
        }
    }

    s_anim_prev_w = w;
    s_anim_prev_h = h;
}
static void fs_anim_timer_cb(lv_timer_t *tmr)
{
    int64_t dur_ms = s_anim_open ? ANIM_FS_MS_OPEN : ANIM_FS_MS_CLOSE;

    int64_t elaps = esp_timer_get_time() - s_anim_t0;  /* us */
    int64_t v = elaps / 1000;                           /* ms 原始进度 */
    if (v > dur_ms) v = dur_ms;
    if (v < 0) v = 0;

    /* 进度曲线: 打开=先快后慢 (ease-out 三次方), 关闭=线性 */
    int64_t p;
    if (s_anim_open) {
        /* p = 1 - (1 - v/dur)^3 */
        double t = (double)v / (double)dur_ms;
        p = (int64_t)((1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t)) * dur_ms);
    } else {
        p = v;
    }

    int w, h;
    int ptw = s_anim_cfg->w, pth = s_anim_cfg->h;
    if (s_anim_open) {
        w = ANIM_FS_START + (int)((int64_t)(ptw - ANIM_FS_START) * p / dur_ms);
        h = ANIM_FS_START + (int)((int64_t)(pth - ANIM_FS_START) * p / dur_ms);
    } else {
        w = ptw - (int)((int64_t)(ptw - ANIM_FS_START) * p / dur_ms);
        h = pth - (int)((int64_t)(pth - ANIM_FS_START) * p / dur_ms);
    }
    if (w < ANIM_FS_START) w = ANIM_FS_START;
    if (h < ANIM_FS_START) h = ANIM_FS_START;
    if (w > ptw) w = ptw;
    if (h > pth) h = pth;

    int64_t c0 = esp_timer_get_time();
    if (s_anim_first_frame) {
        fs_anim_compose(w, h);   /* 首帧: 全量填充 (buf 未初始化) */
        s_anim_first_frame = false;
        lv_obj_invalidate(s_anim_fs_img);
    } else {
        fs_anim_compose_incr(w, h);
        /* 分块 invalidate: 只推变化区域 (两块矩形, 避免 L 型变方块推全屏) */
        if (s_anim_chg_valid) {
            fs_anim_inv_area(&s_anim_chg_a);
            fs_anim_inv_area(&s_anim_chg_b);
        }
    }
    s_anim_prev_w = w;
    s_anim_prev_h = h;
    s_anim_compose_us = esp_timer_get_time() - c0;

    /* 手动触发同步刷新, 并测量刷屏耗时 (LVGL 内部绘制 + flush) */
    int64_t r0 = esp_timer_get_time();
    lv_refr_now(lv_disp_get_default());
    s_anim_refr_us = esp_timer_get_time() - r0;

#if 0  /* 调试: 每帧瞬时 fps + compose/refr 耗时 (已禁用) */
    int64_t now = esp_timer_get_time();
    double fps = 0;
    if (s_prev_frame_ok && now > s_prev_frame_us) {
        double dt_ms = (double)(now - s_prev_frame_us) / 1000.0;
        fps = dt_ms > 0 ? (1000.0 / dt_ms) : 0;
    }
    s_prev_frame_us = now;
    s_prev_frame_ok = true;
    printf("[ANIM] fps=%5.1f compose=%5lldus refr=%6lldus total=%6lldus (w=%d h=%d)\n",
           fps,
           (long long)s_anim_compose_us, (long long)s_anim_refr_us,
           (long long)(s_anim_compose_us + s_anim_refr_us),
           w, h);
#endif

    if (v >= dur_ms) {
        lv_timer_del(tmr);
        s_anim_timer = NULL;
        if (s_anim_open) {
            /* 打开完成: 真实面板已就绪, 删除顶部图片层并恢复显示 */
            lv_obj_del(s_anim_bg_img);
            lv_obj_del(s_anim_fs_img);
            s_anim_bg_img = NULL;
            s_anim_fs_img = NULL;
            lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        } else {
            /* 关闭完成: 删除顶部图片层 + 真实面板, 恢复主界面 */
            lv_obj_del(s_anim_bg_img);
            lv_obj_del(s_anim_fs_img);
            s_anim_bg_img = NULL;
            s_anim_fs_img = NULL;
            if (s_anim_cfg->close_real) s_anim_cfg->close_real();
            lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        }
        if (s_anim_buf_main) { heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL; }
        if (s_anim_buf_fs)   { heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL; }
        if (s_anim_buf_anim) { heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL; }
        g_anim_active = false;

        /* 动画结束: 恢复 LVGL 自动刷新 */
        lv_timer_t *refr_t = _lv_disp_get_refr_timer(lv_disp_get_default());
        if (refr_t) {
            lv_timer_set_period(refr_t, LV_DISP_DEF_REFR_PERIOD);
            lv_timer_resume(refr_t);
        }
    }
}

static void fs_anim_start(int open)
{
    if (s_anim_timer) {
        lv_timer_del(s_anim_timer);
        s_anim_timer = NULL;
        /* 清理上一个未完成动画的资源 */
        if (s_anim_bg_img) { lv_obj_del(s_anim_bg_img); s_anim_bg_img = NULL; }
        if (s_anim_fs_img) { lv_obj_del(s_anim_fs_img); s_anim_fs_img = NULL; }
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        if (s_anim_buf_main) { heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL; }
        if (s_anim_buf_fs)   { heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL; }
        if (s_anim_buf_anim) { heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL; }
        /* 上一个动画被中断, 恢复自动刷新 */
        lv_timer_t *old_refr = _lv_disp_get_refr_timer(lv_disp_get_default());
        if (old_refr) {
            lv_timer_set_period(old_refr, LV_DISP_DEF_REFR_PERIOD);
            lv_timer_resume(old_refr);
        }
    }
    s_anim_open = open;
    s_anim_t0 = esp_timer_get_time();
    g_anim_active = true;
    /* 每次动画开始: 瞬时 fps 状态复位 (首帧打印 0) */
    s_prev_frame_ok = false;
    s_prev_frame_us = 0;
    /* 打开: 首帧需全量填充 buf (全新分配)。关闭: buf 已在 panel_anim_close 全量填好,
     * prev 初始化为全尺寸, 首帧(全展开)走增量且无变化。 */
    s_anim_first_frame = (open != 0);
    s_anim_prev_w = open ? 0 : s_anim_cfg->w;
    s_anim_prev_h = open ? 0 : s_anim_cfg->h;

    /* 动画期间: 暂停 LVGL 自动刷新, 改由动画定时器手动触发 (测刷屏耗时) */
    lv_timer_t *refr_t = _lv_disp_get_refr_timer(lv_disp_get_default());
    if (refr_t) {
        lv_timer_pause(refr_t);
        lv_timer_set_period(refr_t, 1000);   /* 兜底 1s 1 帧 */
    }

    s_anim_timer = lv_timer_create(fs_anim_timer_cb, 20, NULL);
    lv_timer_ready(s_anim_timer);
}

/* 打开动画通用入口: 在菜单点击回调调用 (此时真实面板尚未创建) */
static void panel_anim_open(const panel_anim_cfg_t *cfg)
{
    s_anim_cfg = cfg;
    int pw = cfg->w, ph = cfg->h;

    /* 1. 主界面截图 (面板未创建, 画面干净) */
    s_anim_buf_main = fs_anim_alloc(172 * 320 * 2);
    if (!s_anim_buf_main) { cfg->open_real(); return; }
    if (lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR,
                                &s_anim_dsc_main, s_anim_buf_main,
                                172 * 320 * 2) != LV_RES_OK) {
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        cfg->open_real();
        return;
    }

    /* 2. 创建真实面板 */
    cfg->open_real();

    /* 3. 面板截图 */
    s_anim_buf_fs = fs_anim_alloc(pw * ph * 2);
    s_anim_buf_anim = fs_anim_alloc(pw * ph * 2);
    if (!s_anim_buf_fs || !s_anim_buf_anim) {
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL;
        heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL;
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (lv_snapshot_take_to_buf(*cfg->cont_ref, LV_IMG_CF_TRUE_COLOR,
                                &s_anim_dsc_fs, s_anim_buf_fs,
                                pw * ph * 2) != LV_RES_OK) {
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL;
        heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL;
        lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* 4. 隐藏主界面 + 面板 (一个 flag, 隐藏即不绘制子树) */
    lv_obj_add_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);

    /* 5. 顶部层: 背景主界面图 */
    s_anim_bg_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_bg_img, &s_anim_dsc_main);
    lv_obj_set_pos(s_anim_bg_img, 0, 0);

    /* 6. 顶部层: 面板展开图 (位于面板坐标) */
    memset(&s_anim_dsc_buf, 0, sizeof(s_anim_dsc_buf));
    s_anim_dsc_buf.header.w = pw;
    s_anim_dsc_buf.header.h = ph;
    s_anim_dsc_buf.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_anim_dsc_buf.data_size = pw * ph * 2;
    s_anim_dsc_buf.data = s_anim_buf_anim;
    s_anim_fs_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_fs_img, &s_anim_dsc_buf);
    lv_obj_set_pos(s_anim_fs_img, cfg->x, cfg->y);

    /* 7. 启动展开动画 */
    fs_anim_start(1);
}

/* 关闭动画通用入口: 在 overlay 点击回调调用 (真实面板可见) */
static void panel_anim_close(const panel_anim_cfg_t *cfg)
{
    if (!*cfg->overlay_ref) return;
    s_anim_cfg = cfg;
    int pw = cfg->w, ph = cfg->h;

    /* 1. 当前面板截图 (可能已翻页/进目录) */
    s_anim_buf_fs = fs_anim_alloc(pw * ph * 2);
    s_anim_buf_anim = fs_anim_alloc(pw * ph * 2);
    s_anim_buf_main = fs_anim_alloc(172 * 320 * 2);
    if (!s_anim_buf_fs || !s_anim_buf_anim || !s_anim_buf_main) {
        if (s_anim_buf_fs)   { heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs = NULL; }
        if (s_anim_buf_anim) { heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL; }
        if (s_anim_buf_main) { heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL; }
        cfg->close_real();
        return;
    }
    if (lv_snapshot_take_to_buf(*cfg->cont_ref, LV_IMG_CF_TRUE_COLOR,
                                &s_anim_dsc_fs, s_anim_buf_fs,
                                pw * ph * 2) != LV_RES_OK) {
        cfg->close_real();
        heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL;
        heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL;
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        return;
    }

    /* 2. 临时隐藏面板 overlay 截图干净主界面, 再恢复 (同帧无闪烁) */
    lv_obj_add_flag(*cfg->overlay_ref, LV_OBJ_FLAG_HIDDEN);
    if (lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR,
                                &s_anim_dsc_main, s_anim_buf_main,
                                172 * 320 * 2) != LV_RES_OK) {
        lv_obj_clear_flag(*cfg->overlay_ref, LV_OBJ_FLAG_HIDDEN);
        cfg->close_real();
        heap_caps_free(s_anim_buf_fs);   s_anim_buf_fs   = NULL;
        heap_caps_free(s_anim_buf_anim); s_anim_buf_anim = NULL;
        heap_caps_free(s_anim_buf_main); s_anim_buf_main = NULL;
        return;
    }
    lv_obj_clear_flag(*cfg->overlay_ref, LV_OBJ_FLAG_HIDDEN);

    /* 3. 隐藏主界面 + 面板 */
    lv_obj_add_flag(lv_scr_act(), LV_OBJ_FLAG_HIDDEN);

    /* 4. 顶部层图片: 背景 + 初始全展开面板 */
    s_anim_bg_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_bg_img, &s_anim_dsc_main);
    lv_obj_set_pos(s_anim_bg_img, 0, 0);

    memset(&s_anim_dsc_buf, 0, sizeof(s_anim_dsc_buf));
    s_anim_dsc_buf.header.w = pw;
    s_anim_dsc_buf.header.h = ph;
    s_anim_dsc_buf.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_anim_dsc_buf.data_size = pw * ph * 2;
    s_anim_dsc_buf.data = s_anim_buf_anim;
    s_anim_fs_img = lv_img_create(lv_layer_top());
    lv_img_set_src(s_anim_fs_img, &s_anim_dsc_buf);
    lv_obj_set_pos(s_anim_fs_img, cfg->x, cfg->y);

    fs_anim_compose(pw, ph);

    /* 5. 启动收拢动画 */
    fs_anim_start(0);
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
        static const panel_anim_cfg_t cfg = {
            .overlay_ref = &s_bt_overlay,
            .cont_ref    = &s_bt_cont,
            .w = BT_W, .h = BT_H,
            .x = ANIM_BT_X, .y = ANIM_BT_Y,
            .anchor_right = true,
            .open_real  = bt_list_open,
            .close_real = bt_list_close,
        };
        panel_anim_close(&cfg);
    } else {
        static const panel_anim_cfg_t cfg = {
            .overlay_ref = &s_bt_overlay,
            .cont_ref    = &s_bt_cont,
            .w = BT_W, .h = BT_H,
            .x = ANIM_BT_X, .y = ANIM_BT_Y,
            .anchor_right = true,
            .open_real  = bt_list_open,
            .close_real = bt_list_close,
        };
        panel_anim_open(&cfg);
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

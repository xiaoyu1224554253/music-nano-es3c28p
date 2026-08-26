#pragma once

#include "lvgl.h"
#include "bt_a2dp.h"

void fs_menu_click_cb(lv_event_t *e);
void fs_browser_on_sd_ready(void);
void fs_browser_on_sd_remove(void);

void bt_list_init(bt_a2dp_iface_t *iface);
void bt_menu_click_cb(lv_event_t *e);
void bt_list_on_device_found(const char *name);
void bt_list_on_scan_done(void);
void bt_list_on_connected(const char *name);
void bt_list_on_connect_failed(const char *name);
void bt_list_on_disconnected(void);
void bt_list_on_state_rsp(bt_state_t state, const char *name);

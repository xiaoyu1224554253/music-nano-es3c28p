#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_timer.h"
#include "bt_a2dp.h"
#include "audio_task.h"
#include "atomic_utils.h"
#include "volume.h"

#define BT_TAG              "BT_A2DP"
#define RC_TAG              "BT_RC"
#define LOCAL_DEVICE_NAME   "ESP_MUSIC"

#define APP_RC_CT_TL_GET_CAPS          (0)
#define APP_RC_CT_TL_RN_VOLUME_CHANGE  (1)

#define CACHE_MAX 16

typedef enum {
    BT_DISPATCH_A2DP,
    BT_DISPATCH_AVRC,
} bt_dispatch_type_t;

typedef struct {
    bt_dispatch_type_t type;
    uint16_t           event;
    void              *param;
} bt_dispatch_msg_t;

static bt_a2dp_iface_t s_iface;
static SemaphoreHandle_t s_init_sem = NULL;
static bool s_bt_ready = false;
static bool s_connected = false;
static bool s_scanning = false;
static bool s_connecting = false;
static bool s_stream_started = false;
static esp_bd_addr_t s_peer_bda;

static uint8_t s_cache_count = 0;
static esp_bd_addr_t s_cache_bda[CACHE_MAX];
static char s_cache_name[CACHE_MAX][32];

static char        s_connect_target[32];
static int         s_connect_retry;
static bool        s_connect_found;
static bool        s_pending_connect_scan = false;
static esp_bd_addr_t s_connect_bda;

static char        s_connecting_name[32];
static char        s_connected_name[32];

static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;

static int32_t s_last_sent_vol = -1; /* 已同步到耳机的音量, -1 表示未同步 */

static QueueHandle_t    s_dispatch_queue = NULL;
static QueueSetHandle_t s_queue_set      = NULL;

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static bool get_name_from_eir(uint8_t *eir, char *bdname, size_t max_len)
{
    uint8_t *name = NULL;
    uint8_t name_len = 0;
    if (!eir) return false;

    name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_len);
    if (!name) {
        name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_len);
    }
    if (name) {
        if (name_len >= max_len) name_len = max_len - 1;
        memcpy(bdname, name, name_len);
        bdname[name_len] = '\0';
        return true;
    }
    return false;
}

static int cache_find(const char *device_name)
{
    for (int i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache_name[i], device_name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool cache_add(esp_bd_addr_t bda, const char *device_name)
{
    if (cache_find(device_name) >= 0) {
        return false;
    }
    if (s_cache_count >= CACHE_MAX) {
        s_cache_count = 0;
    }
    memcpy(s_cache_bda[s_cache_count], bda, ESP_BD_ADDR_LEN);
    strncpy(s_cache_name[s_cache_count], device_name, sizeof(s_cache_name[0]) - 1);
    s_cache_name[s_cache_count][sizeof(s_cache_name[0]) - 1] = '\0';
    s_cache_count++;
    return true;
}

static void send_evt(bt_evt_type_t type, const char *name, int error)
{
    bt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.error = error;
    if (name) {
        strncpy(evt.device_name, name, sizeof(evt.device_name) - 1);
    }
    xQueueSend(s_iface.evt_queue, &evt, 0);
}

/* 回发当前状态给列表, 用于查询应答与被拒命令的校正 */
static void send_state_rsp(void)
{
    bt_evt_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = BT_EVT_STATE_RSP;
    if (s_connected) {
        evt.state = BT_STATE_CONNECTED;
        strncpy(evt.device_name, s_connected_name, sizeof(evt.device_name) - 1);
    } else if (s_connecting) {
        evt.state = BT_STATE_CONNECTING;
        strncpy(evt.device_name, s_connecting_name, sizeof(evt.device_name) - 1);
    } else {
        evt.state = BT_STATE_DISCONNECTED;
    }
    xQueueSend(s_iface.evt_queue, &evt, 0);
}

static bool bt_a2dp_send_dispatch(bt_dispatch_type_t type, uint16_t event, void *param, size_t param_len)
{
    bt_dispatch_msg_t msg;
    msg.type  = type;
    msg.event = event;
    msg.param = malloc(param_len);
    if (!msg.param) return false;
    memcpy(msg.param, param, param_len);
    if (xQueueSend(s_dispatch_queue, &msg, 0) != pdTRUE) {
        free(msg.param);
        return false;
    }
    return true;
}

/* ── AVRCP notify handler ── */
static void bt_a2dp_volume_changed(void)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_VOLUME_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(APP_RC_CT_TL_RN_VOLUME_CHANGE,
                                                    ESP_AVRC_RN_VOLUME_CHANGE, 0);
    }
}

static void bt_a2dp_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id) {
    case ESP_AVRC_RN_VOLUME_CHANGE: {
        ESP_LOGI(RC_TAG, "音量已变化: %d", event_parameter->volume);
        /* 耳机端音量已生效, 同步全局并标记已同步, 避免 5Hz 轮询回环 */
        volume_set(event_parameter->volume);
        s_last_sent_vol = event_parameter->volume;
        bt_a2dp_volume_changed();
        break;
    }
    default:
        break;
    }
}

/* ── AVRCP event handler ── */
static void bt_a2dp_hdl_avrc_evt(uint16_t event, void *p_param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)p_param;

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
        uint8_t *bda = rc->conn_stat.remote_bda;
        ESP_LOGI(RC_TAG, "AVRC 连接状态事件: 状态 %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        if (rc->conn_stat.connected) {
            esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);
            /* 连接后第一时间同步当前音量到耳机 */
            int32_t v = volume_get();
            s_last_sent_vol = v;
            esp_avrc_ct_send_set_absolute_volume_cmd(APP_RC_CT_TL_GET_CAPS, (uint8_t)v);
        } else {
            s_avrc_peer_rn_cap.bits = 0;
            s_last_sent_vol = -1;
        }
        break;
    }
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        ESP_LOGI(RC_TAG, "AVRC 元数据响应: 属性ID 0x%x, %s", rc->meta_rsp.attr_id, rc->meta_rsp.attr_text);
        free(rc->meta_rsp.attr_text);
        break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
        bt_a2dp_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
        break;
    }
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {
        ESP_LOGI(RC_TAG, "AVRC 远程功能 %"PRIx32", 目标功能 %x",
                 rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
        break;
    }
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;
        ESP_LOGI(RC_TAG, "远程通知能力: 数量 %d, 位掩码 0x%x",
                 rc->get_rn_caps_rsp.cap_count, rc->get_rn_caps_rsp.evt_set.bits);
        bt_a2dp_volume_changed();
        break;
    }
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT: {
        ESP_LOGI(RC_TAG, "设置绝对音量响应: %d", rc->set_volume_rsp.volume);
        /* 耳机确认后的实际音量(可能被钳位) */
        volume_set(rc->set_volume_rsp.volume);
        s_last_sent_vol = rc->set_volume_rsp.volume;
        break;
    }
    default:
        break;
    }
}

/* ── A2DP data callback ── */
static uint32_t s_cb_call_count    = 0;
static uint64_t s_cb_total_bytes   = 0;
static uint64_t s_cb_total_got     = 0;
static int64_t  s_cb_last_print_us = 0;

/* 欠载诊断: pcm_stream 不够取时记录 (已禁用, 调试用) */
#if 0
extern volatile bool g_anim_active;
static int64_t  s_cb_last_underrun_us = 0;
static uint32_t s_cb_underrun_count   = 0;
static uint32_t s_cb_underrun_empty   = 0;
#endif

static int32_t bt_a2dp_data_cb(uint8_t *data, int32_t len)
{
    if (!data || len <= 0) {
        return 0;
    }

    s_cb_call_count++;

    if (!s_connected) {
        memset(data, 0, (size_t)len);
        return len;
    }

    size_t got = xStreamBufferReceive(s_iface.pcm_stream, data, (size_t)len, 0);
    if (got < (size_t)len) {
        memset(data + got, 0, (size_t)len - got);
#if 0
        s_cb_underrun_count++;
        int64_t now = esp_timer_get_time();
        if ((now - s_cb_last_underrun_us) >= 50000LL) {
            s_cb_last_underrun_us = now;
            size_t avail = xStreamBufferBytesAvailable(s_iface.pcm_stream);
            if (avail == 0) s_cb_underrun_empty++;
        }
#endif
    }

    s_cb_total_bytes += (uint64_t)len;
    s_cb_total_got   += (uint64_t)got;

    int64_t now = esp_timer_get_time();
    if ((now - s_cb_last_print_us) >= 5000000LL) {
        s_cb_last_print_us = now;
        /* [SPI 测量期间] BT_CB 汇总打印已禁用 */
    }

    return len;
}

/* ── A2DP event handler ── */
static void bt_a2dp_hdl_a2d_evt(uint16_t event, void *p_param)
{
    esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)p_param;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_connected = true;
            s_connecting = false;
            s_stream_started = false;
            strncpy(s_connected_name, s_connecting_name,
                    sizeof(s_connected_name) - 1);
            s_connected_name[sizeof(s_connected_name) - 1] = '\0';
            memset(s_connecting_name, 0, sizeof(s_connecting_name));
            ESP_LOGI(BT_TAG, "A2DP 已连接");
            send_evt(BT_EVT_CONNECTED, s_connected_name, 0);
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_connected = false;
            s_connecting = false;
            s_stream_started = false;
            memset(s_connected_name, 0, sizeof(s_connected_name));
            memset(s_connecting_name, 0, sizeof(s_connecting_name));
            ESP_LOGI(BT_TAG, "A2DP 已断开连接");
            send_evt(BT_EVT_DISCONNECTED, NULL, 0);
            send_evt(BT_EVT_STREAM_STOPPED, NULL, 0);
        }
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
            a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(BT_TAG, "A2DP 流媒体已启动");
            send_evt(BT_EVT_STREAM_READY, NULL, 0);
        } else if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND &&
                   a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(BT_TAG, "A2DP 流媒体已挂起");
            send_evt(BT_EVT_STREAM_STOPPED, NULL, 0);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        ESP_LOGI(BT_TAG, "延迟值: %u * 1/10 毫秒", a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default:
        break;
    }
}

/* ── GAP callback (called from BT context, keep it fast) ── */
static void bt_a2dp_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];

    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        if (!s_scanning) break;

        uint32_t cod = 0;
        uint8_t *eir = NULL;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = param->disc_res.prop + i;
            switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_COD:
                cod = *(uint32_t *)(p->val);
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                eir = (uint8_t *)(p->val);
                break;
            default:
                break;
            }
        }

        if (!esp_bt_gap_is_valid_cod(cod) ||
            !(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
            break;
        }

        char device_name[32] = {0};
        if (eir && get_name_from_eir(eir, device_name, sizeof(device_name))) {
            bda2str(param->disc_res.bda, bda_str, sizeof(bda_str));
            ESP_LOGI(BT_TAG, "扫描到设备: %s, 名称 %s", bda_str, device_name);

            if (s_connect_target[0] != '\0') {
                if (strcmp(device_name, s_connect_target) == 0) {
                    s_connect_found = true;
                    memcpy(s_connect_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
                    esp_bt_gap_cancel_discovery();
                }
            } else {
                if (cache_add(param->disc_res.bda, device_name)) {
                    send_evt(BT_EVT_DEVICE_FOUND, device_name, 0);
                }
            }
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_scanning = false;

            if (s_connect_target[0] != '\0') {
                if (s_connect_found) {
                    ESP_LOGI(BT_TAG, "找到目标设备 %s，开始连接", s_connect_target);
                    memcpy(s_peer_bda, s_connect_bda, ESP_BD_ADDR_LEN);
                    esp_a2d_source_connect(s_peer_bda);
                    memset(s_connect_target, 0, sizeof(s_connect_target));
                } else if (s_pending_connect_scan) {
                    s_pending_connect_scan = false;
                    ESP_LOGI(BT_TAG, "空闲扫描已取消，开始连接扫描: %s", s_connect_target);
                    s_scanning = true;
                    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                               1, 0);
                } else if (s_connect_retry < 4) {
                    s_connect_retry++;
                    ESP_LOGI(BT_TAG, "未找到设备 %s，第 %d 次重试",
                             s_connect_target, s_connect_retry);
                    s_scanning = true;
                    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                               3, 0);
                } else {
                    ESP_LOGW(BT_TAG, "未找到设备 %s，连接失败", s_connect_target);
                    s_connecting = false;
                    memset(s_connecting_name, 0, sizeof(s_connecting_name));
                    send_evt(BT_EVT_CONNECT_FAILED, s_connect_target, -1);
                    memset(s_connect_target, 0, sizeof(s_connect_target));
                }
            } else {
                ESP_LOGI(BT_TAG, "设备搜索已停止。");
                send_evt(BT_EVT_SCAN_DONE, NULL, 0);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            s_scanning = true;
            ESP_LOGI(BT_TAG, "设备搜索已开始。");
        }
        break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_TAG, "认证成功: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(BT_TAG, "认证失败, 状态: %d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
        if (param->pin_req.min_16_digit) {
            esp_bt_pin_code_t pin_code = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            esp_bt_pin_code_t pin_code;
            pin_code[0] = '1';
            pin_code[1] = '2';
            pin_code[2] = '3';
            pin_code[3] = '4';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(BT_TAG, "请比对数字: %06"PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(BT_TAG, "配对密钥: %06"PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(BT_TAG, "请输入配对密钥!");
        break;
    default:
        break;
    }
}

/* ── A2DP callback -> dispatch to internal queue ── */
static void bt_a2dp_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_a2dp_send_dispatch(BT_DISPATCH_A2DP, event, param, sizeof(esp_a2d_cb_param_t));
}

/* ── AVRCP callback -> dispatch to internal queue ── */
static void bt_a2dp_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    bt_a2dp_send_dispatch(BT_DISPATCH_AVRC, event, param, sizeof(esp_avrc_ct_cb_param_t));
}

/* ── Stack up handler ── */
static void bt_a2dp_hdl_stack_up(void)
{
    char *dev_name = LOCAL_DEVICE_NAME;
    esp_bt_gap_set_device_name(dev_name);
    esp_bt_gap_register_callback(bt_a2dp_gap_cb);

    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(bt_a2dp_rc_ct_cb);

    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evt_set);

    esp_a2d_source_init();
    esp_a2d_register_callback(&bt_a2dp_a2d_cb);
    esp_a2d_source_register_data_callback(bt_a2dp_data_cb);

    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    s_bt_ready = true;
    ESP_LOGI(BT_TAG, "蓝牙初始化完成");
}

/* ── 未连接时排水: 以 44.1kHz/16bit/双声道 真实速率丢弃 PCM ── */
#define DRAIN_BYTES_PER_SEC  (44100 * 2 * 2)

/* 5Hz 轮询: 全局音量有变化则同步到耳机 */
static void bt_sync_volume(void)
{
    int32_t v = volume_get();
    if (v != s_last_sent_vol) {
        s_last_sent_vol = v;
        esp_avrc_ct_send_set_absolute_volume_cmd(APP_RC_CT_TL_GET_CAPS, (uint8_t)v);
    }
}

static void bt_drain_pcm(void)
{
    static int64_t s_drain_last_us = 0;
    int64_t now = esp_timer_get_time();
    if (s_drain_last_us == 0) {
        s_drain_last_us = now;
        return;
    }
    int64_t dt = now - s_drain_last_us;
    s_drain_last_us = now;
    if (dt <= 0) return;

    uint32_t budget = (uint32_t)(((uint64_t)DRAIN_BYTES_PER_SEC * (uint64_t)dt) / 1000000ULL);

    uint8_t tmp[256];
    while (budget > 0) {
        size_t want = budget > sizeof(tmp) ? sizeof(tmp) : budget;
        size_t got = xStreamBufferReceive(s_iface.pcm_stream, tmp, want, 0);
        if (got == 0) break;
        budget -= got;
    }
}

/* ── BT task entry (merged: commands + dispatch events) ── */
static void bt_a2dp_task(void *arg)
{
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(BT_TAG, "蓝牙控制器初始化失败");
        xSemaphoreGive(s_init_sem);
        vTaskDelete(NULL);
        return;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(BT_TAG, "蓝牙控制器启用失败");
        xSemaphoreGive(s_init_sem);
        vTaskDelete(NULL);
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_TAG, "Bluedroid 初始化失败: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_init_sem);
        vTaskDelete(NULL);
        return;
    }
    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(BT_TAG, "Bluedroid 启用失败");
        xSemaphoreGive(s_init_sem);
        vTaskDelete(NULL);
        return;
    }

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    esp_bt_gap_set_pin(pin_type, 0, pin_code);

    char bda_str[18];
    ESP_LOGI(BT_TAG, "本机地址:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));

    s_iface.cmd_queue  = xQueueCreate(10, sizeof(bt_cmd_t));
    s_iface.evt_queue  = xQueueCreate(20, sizeof(bt_evt_t));
    s_iface.pcm_stream = xStreamBufferCreate(16 * 1024, 512);

    s_dispatch_queue = xQueueCreate(10, sizeof(bt_dispatch_msg_t));
    s_queue_set      = xQueueCreateSet(8);
    xQueueAddToSet(s_iface.cmd_queue, s_queue_set);
    xQueueAddToSet(s_dispatch_queue, s_queue_set);

    bt_a2dp_hdl_stack_up();

    xSemaphoreGive(s_init_sem);

    bt_cmd_t          cmd;
    bt_dispatch_msg_t disp;
    QueueHandle_t     active;

    while (1) {
        if (s_connected) {
            active = xQueueSelectFromSet(s_queue_set, pdMS_TO_TICKS(200));
            bt_sync_volume();
            if (active == NULL) {
                bool want = atomic_load_bool(&g_pcm_active);
                if (want != s_stream_started) {
                    esp_a2d_media_ctrl(want ? ESP_A2D_MEDIA_CTRL_START
                                            : ESP_A2D_MEDIA_CTRL_SUSPEND);
                    s_stream_started = want;
                }
                continue;
            }
        } else {
            /* 未连接(含连接中): 仅排水(纯 CPU)。调制解调器睡眠由控制器自动管理, 不手动开关 */
            bool have_pcm = atomic_load_bool(&g_pcm_active) ||
                            xStreamBufferBytesAvailable(s_iface.pcm_stream) > 0;
            active = xQueueSelectFromSet(s_queue_set,
                                         have_pcm ? pdMS_TO_TICKS(100)
                                                  : pdMS_TO_TICKS(1000));
            if (active == NULL) {
                if (have_pcm) {
                    bt_drain_pcm();
                }
                continue;
            }
        }

        if (active == s_iface.cmd_queue) {
            xQueueReceive(s_iface.cmd_queue, &cmd, 0);

            switch (cmd.type) {
            case BT_CMD_SCAN: {
                if (s_connected) {
                    ESP_LOGW(BT_TAG, "已连接设备，忽略扫描命令");
                    send_state_rsp();
                    break;
                }
                if (s_scanning) {
                    ESP_LOGW(BT_TAG, "正在扫描中，忽略重复扫描命令");
                    break;
                }
                if (s_connect_target[0] != '\0') {
                    ESP_LOGW(BT_TAG, "正在进行连接扫描，忽略扫描命令");
                    break;
                }
                ESP_LOGI(BT_TAG, "开始设备搜索...");
                s_cache_count = 0;
                memset(s_cache_bda, 0, sizeof(s_cache_bda));
                memset(s_cache_name, 0, sizeof(s_cache_name));
                s_scanning = true;
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 3, 0);
                break;
            }
            case BT_CMD_STOP_SCAN: {
                if (!s_scanning) {
                    ESP_LOGW(BT_TAG, "未在扫描中，忽略终止扫描命令");
                    send_state_rsp();
                    break;
                }
                ESP_LOGI(BT_TAG, "终止扫描...");
                if (s_connect_target[0] != '\0') {
                    memset(s_connect_target, 0, sizeof(s_connect_target));
                    s_pending_connect_scan = false;
                    s_connecting = false;
                    memset(s_connecting_name, 0, sizeof(s_connecting_name));
                    send_evt(BT_EVT_CONNECT_FAILED, NULL, -1);
                }
                esp_bt_gap_cancel_discovery();
                break;
            }
            case BT_CMD_CONNECT: {
                if (s_connected) {
                    ESP_LOGW(BT_TAG, "已连接设备，忽略连接命令");
                    break;
                }
                if (s_connect_target[0] != '\0') {
                    ESP_LOGW(BT_TAG, "正在连接中，忽略重复连接命令");
                    break;
                }
                ESP_LOGI(BT_TAG, "开始搜索并连接: %s", cmd.device_name);
                strncpy(s_connect_target, cmd.device_name,
                        sizeof(s_connect_target) - 1);
                s_connect_target[sizeof(s_connect_target) - 1] = '\0';
                strncpy(s_connecting_name, cmd.device_name,
                        sizeof(s_connecting_name) - 1);
                s_connecting_name[sizeof(s_connecting_name) - 1] = '\0';
                s_connect_retry = 0;
                s_connect_found = false;
                s_connecting = true;

                if (s_scanning) {
                    s_pending_connect_scan = true;
                    esp_bt_gap_cancel_discovery();
                } else {
                    s_scanning = true;
                    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 1, 0);
                }
                break;
            }
            case BT_CMD_DISCONNECT: {
                if (!s_connected) {
                    ESP_LOGW(BT_TAG, "未连接设备，忽略断开命令");
                    break;
                }
                ESP_LOGI(BT_TAG, "正在断开连接...");
                esp_a2d_source_disconnect(s_peer_bda);
                break;
            }
            case BT_CMD_GET_STATE: {
                send_state_rsp();
                break;
            }
            default:
                break;
            }
        } else if (active == s_dispatch_queue) {
            xQueueReceive(s_dispatch_queue, &disp, 0);

            switch (disp.type) {
            case BT_DISPATCH_A2DP:
                bt_a2dp_hdl_a2d_evt(disp.event, disp.param);
                break;
            case BT_DISPATCH_AVRC:
                bt_a2dp_hdl_avrc_evt(disp.event, disp.param);
                break;
            }

            if (disp.param) {
                free(disp.param);
            }
        }
    }
}

bt_a2dp_iface_t *bt_a2dp_init(void)
{
    s_init_sem = xSemaphoreCreateBinary();
    if (!s_init_sem) {
        return NULL;
    }

    BaseType_t result = xTaskCreatePinnedToCore(bt_a2dp_task, "bt_a2dp", 3072, NULL, 1, NULL, 0);
    if (result != pdPASS) {
        vSemaphoreDelete(s_init_sem);
        s_init_sem = NULL;
        return NULL;
    }

    if (xSemaphoreTake(s_init_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        vSemaphoreDelete(s_init_sem);
        s_init_sem = NULL;
        return NULL;
    }

    vSemaphoreDelete(s_init_sem);
    s_init_sem = NULL;
    return &s_iface;
}

bt_state_t bt_a2dp_get_state(void)
{
    if (!s_bt_ready) return BT_STATE_DISCONNECTED;
    if (s_connected) return BT_STATE_CONNECTED;
    if (s_connecting) return BT_STATE_CONNECTING;
    return BT_STATE_DISCONNECTED;
}

bool bt_a2dp_is_connected(void)
{
    return s_connected && s_bt_ready;
}

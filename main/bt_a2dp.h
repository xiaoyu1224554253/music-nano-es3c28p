#ifndef __BT_A2DP_H__
#define __BT_A2DP_H__

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_CMD_SCAN       = 0,
    BT_CMD_CONNECT    = 1,
    BT_CMD_DISCONNECT = 2,
} bt_cmd_type_t;

typedef struct {
    bt_cmd_type_t type;
    char          device_name[32];
} bt_cmd_t;

typedef enum {
    BT_EVT_DEVICE_FOUND    = 0,
    BT_EVT_SCAN_DONE       = 1,
    BT_EVT_CONNECTED       = 2,
    BT_EVT_CONNECT_FAILED  = 3,
    BT_EVT_DISCONNECTED    = 4,
} bt_evt_type_t;

typedef struct {
    bt_evt_type_t type;
    char          device_name[32];
    int           error;
} bt_evt_t;

typedef struct {
    QueueHandle_t        cmd_queue;
    QueueHandle_t        evt_queue;
    StreamBufferHandle_t pcm_stream;
} bt_a2dp_iface_t;

bt_a2dp_iface_t *bt_a2dp_init(void);

bool bt_a2dp_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif

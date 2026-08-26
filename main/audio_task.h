#ifndef __AUDIO_TASK_H__
#define __AUDIO_TASK_H__

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

extern volatile bool g_pcm_active;

typedef enum {
    AUDIO_CMD_PLAY            = 0,
    AUDIO_CMD_STOP            = 1,
    AUDIO_CMD_PAUSE           = 2,
    AUDIO_CMD_BT_CONNECTED    = 3,
    AUDIO_CMD_BT_DISCONNECTED = 4,
    AUDIO_CMD_SEEK            = 5,
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t type;
    char             path[256];
    uint32_t         param;
} audio_cmd_t;

typedef enum {
    AUDIO_RSP_BT_CHECK       = 0,
    AUDIO_RSP_FILE_NOT_FOUND = 1,
    AUDIO_RSP_SONG_FINISHED  = 2,
} audio_rsp_type_t;

typedef struct {
    audio_rsp_type_t type;
} audio_rsp_t;

typedef struct {
    QueueHandle_t        cmd_queue;
    QueueHandle_t        rsp_queue;
    StreamBufferHandle_t pcm_stream;
} audio_task_params_t;

void audio_task_init(const audio_task_params_t *params);

#ifdef __cplusplus
}
#endif

#endif

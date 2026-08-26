#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "audio_task.h"
#include "music_data.h"

#define AUDIO_TAG "AUDIO"

#define PCM_TIMEOUT_MS_MAX 100

typedef enum {
    STATE_IDLE = 0,
    STATE_PLAYING,
    STATE_CHECKING,
} audio_state_t;

static QueueHandle_t        s_cmd_queue  = NULL;
static QueueHandle_t        s_rsp_queue  = NULL;
static StreamBufferHandle_t s_pcm_stream = NULL;
static audio_state_t        s_state      = STATE_IDLE;
static bool                 s_was_playing = false;
static uint32_t             s_timeout_ms  = 0;
static bool                 s_pending_pcm = false;
static int16_t              s_pcm_buf[882];
static uint32_t             s_pcm_pos     = 0;

static void audio_task(void *arg)
{
    audio_cmd_t cmd;
    audio_rsp_t rsp;

    while (1) {
        switch (s_state) {

        case STATE_IDLE:
            if (xQueueReceive(s_cmd_queue, &cmd, 0) != pdTRUE) {
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }

            switch (cmd.type) {
            case AUDIO_CMD_PLAY:
                s_state = STATE_PLAYING;
                s_timeout_ms = 0;
                s_was_playing = true;
                printf("[音频] 开始播放\n");
                break;

            case AUDIO_CMD_BT_CONNECTED:
                if (s_was_playing) {
                    s_state = STATE_PLAYING;
                    s_timeout_ms = 0;
                }
                break;

            case AUDIO_CMD_STOP:
            case AUDIO_CMD_PAUSE:
                s_was_playing = false;
                s_pending_pcm = false;
                xStreamBufferReset(s_pcm_stream);
                printf("[音频] %s\n", cmd.type == AUDIO_CMD_STOP ? "停止" : "暂停");
                break;

            default:
                break;
            }
            break;

        case STATE_PLAYING: {
            if (!s_pending_pcm) {
                const int samples = 441;
                for (int i = 0; i < samples; i++) {
                    int16_t val = music_pcm[s_pcm_pos % MUSIC_PCM_LEN];
                    s_pcm_buf[i * 2]     = val;
                    s_pcm_buf[i * 2 + 1] = val;
                    s_pcm_pos++;
                }
                s_pending_pcm = true;
            }

            const size_t needed = sizeof(s_pcm_buf);
            if (xStreamBufferSpacesAvailable(s_pcm_stream) >= needed) {
                xStreamBufferSend(s_pcm_stream, s_pcm_buf, needed, 0);
                s_timeout_ms = 0;
                s_pending_pcm = false;
            } else {
                s_timeout_ms += PCM_TIMEOUT_MS_MAX / 10;
                if (s_timeout_ms >= PCM_TIMEOUT_MS_MAX) {
                    s_timeout_ms = 0;
                    rsp.type = AUDIO_RSP_BT_CHECK;
                    xQueueSend(s_rsp_queue, &rsp, 0);
                    s_state = STATE_CHECKING;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
                switch (cmd.type) {
                case AUDIO_CMD_STOP:
                    xStreamBufferReset(s_pcm_stream);
                    s_pending_pcm = false;
                    s_state = STATE_IDLE;
                    s_was_playing = false;
                    printf("[音频] 停止\n");
                    break;
                case AUDIO_CMD_PAUSE:
                    xStreamBufferReset(s_pcm_stream);
                    s_pending_pcm = false;
                    s_state = STATE_IDLE;
                    printf("[音频] 暂停\n");
                    break;
                case AUDIO_CMD_BT_DISCONNECTED:
                    xStreamBufferReset(s_pcm_stream);
                    s_pending_pcm = false;
                    s_state = STATE_IDLE;
                    printf("[音频] BT 断开, 停止播放\n");
                    break;
                default:
                    break;
                }
                if (s_state != STATE_PLAYING) break;
            }

            break;
        }

        case STATE_CHECKING:
            if (xQueueReceive(s_cmd_queue, &cmd, 0) != pdTRUE) {
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }

            switch (cmd.type) {
            case AUDIO_CMD_BT_CONNECTED:
                s_state = STATE_PLAYING;
                s_timeout_ms = 0;
                break;
            case AUDIO_CMD_BT_DISCONNECTED:
                s_pending_pcm = false;
                s_state = STATE_IDLE;
                break;
            case AUDIO_CMD_STOP:
                s_pending_pcm = false;
                s_state = STATE_IDLE;
                s_was_playing = false;
                printf("[音频] 停止\n");
                break;
            default:
                break;
            }
            break;
        }

    }
}

void audio_task_init(const audio_task_params_t *params)
{
    s_cmd_queue  = params->cmd_queue;
    s_rsp_queue  = params->rsp_queue;
    s_pcm_stream = params->pcm_stream;

    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 6, NULL, 0);
}

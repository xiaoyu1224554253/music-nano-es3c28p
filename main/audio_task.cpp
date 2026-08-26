#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <cstdlib>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_task.h"
#include "decoder.h"

#define AUDIO_TAG "AUDIO"

#define PCM_DEADLINE_US    100000

/* MPEG1 stereo: 1152 samples × 2ch = 2304 int16_t */
#define MP3_PCM_BUF_SAMPLES  (1152 * 2)

typedef enum {
    STATE_IDLE = 0,
    STATE_PLAYING,
    STATE_CHECKING,
} audio_state_t;

static QueueHandle_t        s_cmd_queue   = NULL;
static QueueHandle_t        s_rsp_queue   = NULL;
static StreamBufferHandle_t s_pcm_stream  = NULL;
static audio_state_t        s_state       = STATE_IDLE;
static bool                 s_was_playing = false;

static audio_decoder_t     *s_decoder     = NULL;

/* ── PCM 输出 ── */
static bool     s_pending_pcm = false;
static int16_t  s_pcm_buf[MP3_PCM_BUF_SAMPLES];
static size_t   s_pcm_bytes   = 0;
static size_t   s_pcm_offset  = 0;

static uint64_t s_total_decoded = 0;
static uint64_t s_total_sent    = 0;
static int64_t  s_play_start_us = 0;

/* ──────────────────────────────────────────────
 *  音频任务
 * ────────────────────────────────────────────── */
static void audio_task(void *arg)
{
    audio_cmd_t cmd;
    audio_rsp_t rsp;

    while(1){
        switch(s_state){

        case STATE_IDLE:
            if(xQueueReceive(s_cmd_queue,&cmd,0)!=pdTRUE){vTaskDelay(pdMS_TO_TICKS(10));break;}

            switch(cmd.type){
            case AUDIO_CMD_PLAY:
                if(s_decoder){
                    /* 已有解码器, 可能是 PAUSE 恢复 */
                } else {
                    s_decoder = decoder_mp3_create();
                    if(!s_decoder){printf("[音频] 创建解码器失败\n");break;}
                    if(!s_decoder->open(s_decoder, cmd.path)){
                        printf("[音频] 无法打开 %s\n",cmd.path);
                        free(s_decoder);
                        s_decoder=NULL;
                        break;
                    }
                    printf("[音频] 解码器已加载\n");
                }
                s_state=STATE_PLAYING; s_was_playing=true; s_pending_pcm=false;
                s_total_decoded=0; s_total_sent=0; s_play_start_us=esp_timer_get_time();
                printf("[音频] 进入STATE_PLAYING | ts=%lld us\n", s_play_start_us);
                break;

            case AUDIO_CMD_BT_CONNECTED:
                if(s_was_playing){s_state=STATE_PLAYING;} break;

            case AUDIO_CMD_STOP:
            case AUDIO_CMD_PAUSE:
                s_was_playing=false; s_pending_pcm=false;
                xStreamBufferReset(s_pcm_stream);
                if(cmd.type==AUDIO_CMD_STOP && s_decoder){
                    s_decoder->close(s_decoder);
                    free(s_decoder);
                    s_decoder=NULL;
                }
                break;

            default: break;
            }
            break;

        /* ─── 播放中 ─── */
        case STATE_PLAYING: {
            if(!s_pending_pcm){
                if(!s_decoder){
                    s_state=STATE_IDLE;
                    break;
                }
                if(!s_decoder->decode(s_decoder, s_pcm_buf, &s_pcm_bytes)){
                    if(s_decoder->is_eof(s_decoder)){
                        int64_t elapsed = esp_timer_get_time() - s_play_start_us;
                        printf("[音频] 播放完毕 | 解码=%" PRIu64 " 发送=%" PRIu64 " 耗时=%lld us (%.2f s)\n",
                               s_total_decoded, s_total_sent, elapsed, elapsed / 1000000.0);
                        s_decoder->close(s_decoder);
                        free(s_decoder);
                        s_decoder=NULL;
                        s_state=STATE_IDLE;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                    break;
                }
                s_total_decoded += s_pcm_bytes;
                s_pending_pcm=true;
                s_pcm_offset=0;
            }

            {
                int64_t deadline = esp_timer_get_time() + PCM_DEADLINE_US;
                bool timed_out = false;

                while(s_pcm_offset < s_pcm_bytes){
                    size_t written = xStreamBufferSend(s_pcm_stream,
                        (const uint8_t*)s_pcm_buf + s_pcm_offset,
                        s_pcm_bytes - s_pcm_offset, 0);

                    vTaskDelay(pdMS_TO_TICKS(5));

                    if(written > 0){
                        s_pcm_offset += written;
                        deadline = esp_timer_get_time() + PCM_DEADLINE_US;
                        continue;
                    }


                    if(esp_timer_get_time() > deadline){
                        printf("[音频] PCM超时! 已写=%zu/%zu 累计发送=%" PRIu64 "\n",
                               s_pcm_offset, s_pcm_bytes, s_total_sent);
                        rsp.type=AUDIO_RSP_BT_CHECK;
                        xQueueSend(s_rsp_queue,&rsp,0);
                        s_state=STATE_CHECKING;
                        timed_out=true;
                        break;
                    }

                    while(xQueueReceive(s_cmd_queue,&cmd,0)==pdTRUE){
                        switch(cmd.type){
                        case AUDIO_CMD_STOP:
                            xStreamBufferReset(s_pcm_stream); s_pending_pcm=false;
                            if(s_decoder){s_decoder->close(s_decoder);free(s_decoder);s_decoder=NULL;}
                            s_state=STATE_IDLE; s_was_playing=false; break;
                        case AUDIO_CMD_PAUSE:
                            xStreamBufferReset(s_pcm_stream); s_pending_pcm=false;
                            s_state=STATE_IDLE; break;
                        case AUDIO_CMD_BT_DISCONNECTED:
                            xStreamBufferReset(s_pcm_stream); s_pending_pcm=false;
                            if(s_decoder){s_decoder->close(s_decoder);free(s_decoder);s_decoder=NULL;}
                            s_state=STATE_IDLE; break;
                        default: break;
                        }
                        if(s_state!=STATE_PLAYING)break;
                    }
                    if(s_state!=STATE_PLAYING)break;
                }

                if(timed_out || s_state!=STATE_PLAYING) break;

                s_total_sent += s_pcm_bytes;
                s_pending_pcm=false;
            }

            while(xQueueReceive(s_cmd_queue,&cmd,0)==pdTRUE){
                switch(cmd.type){
                case AUDIO_CMD_STOP:
                    xStreamBufferReset(s_pcm_stream); s_pending_pcm=false;
                    if(s_decoder){s_decoder->close(s_decoder);free(s_decoder);s_decoder=NULL;}
                    s_state=STATE_IDLE; s_was_playing=false; break;
                case AUDIO_CMD_PAUSE:
                    xStreamBufferReset(s_pcm_stream); s_pending_pcm=false;
                    s_state=STATE_IDLE; break;
                case AUDIO_CMD_BT_DISCONNECTED:
                    xStreamBufferReset(s_pcm_stream); s_pending_pcm=false;
                    if(s_decoder){s_decoder->close(s_decoder);free(s_decoder);s_decoder=NULL;}
                    s_state=STATE_IDLE; break;
                default: break;
                }
                if(s_state!=STATE_PLAYING)break;
            }
            break;
        }

        case STATE_CHECKING:
            if(xQueueReceive(s_cmd_queue,&cmd,0)!=pdTRUE){vTaskDelay(pdMS_TO_TICKS(10));break;}
            switch(cmd.type){
            case AUDIO_CMD_BT_CONNECTED: s_state=STATE_PLAYING; break;
            case AUDIO_CMD_BT_DISCONNECTED:
                if(s_decoder){s_decoder->close(s_decoder);free(s_decoder);s_decoder=NULL;}
                s_state=STATE_IDLE; break;
            case AUDIO_CMD_STOP:
                if(s_decoder){s_decoder->close(s_decoder);free(s_decoder);s_decoder=NULL;}
                s_state=STATE_IDLE; s_was_playing=false; break;
            default: break;
            }
            break;
        }
    }
}

extern "C" void audio_task_init(const audio_task_params_t *params)
{
    s_cmd_queue=params->cmd_queue;
    s_rsp_queue=params->rsp_queue;
    s_pcm_stream=params->pcm_stream;
    xTaskCreatePinnedToCore(audio_task,"audio",4096,NULL,1,NULL,0);
}

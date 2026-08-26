#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "micro_mp3/mp3_decoder.h"
#include "decoder.h"

#define MP3_TAG "DEC_MP3"
#define MP3_INPUT_CHUNK_SIZE 2048
#define MP3_PCM_BUF_SAMPLES  (1152 * 2)

#define ID3_FRAME_TITLE   "TIT2"
#define ID3_FRAME_ARTIST  "TPE1"
#define ID3_FRAME_ALBUM   "TALB"
#define ID3_FRAME_COVER   "APIC"

typedef struct {
    audio_decoder_t iface;

    FILE                  *file;
    micro_mp3::Mp3Decoder *mp3;
    uint8_t               *inbuf;
    size_t                 in_off;
    size_t                 in_len;
    bool                   eof;
    bool                   info_done;
    int16_t                pcm_buf[MP3_PCM_BUF_SAMPLES];

    uint32_t sample_rate;
    uint8_t  channels;
} decoder_mp3_t;

static uint32_t syncsafe(const uint8_t *p)
{
    return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14)
         | ((uint32_t)p[2] <<  7) |  (uint32_t)p[3];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static void parse_id3v2(FILE *f, long *off)
{
    *off = 0;
    uint8_t h[10];
    if(fread(h,1,10,f)!=10 || memcmp(h,"ID3",3)) { fseek(f,0,SEEK_SET); return; }
    uint8_t v=h[3]; bool ft=(h[4]&0x10); uint32_t ts=syncsafe(h+6);
    ESP_LOGI(MP3_TAG, "ID3v2.%u 标签, 大小 %" PRIu32, v, ts);
    uint32_t pos=10;
    while(pos<ts){
        uint8_t fh[10]; if(fread(fh,1,10,f)!=10) break; pos+=10;
        if(fh[0]==0){fseek(f,-10,SEEK_CUR);break;}
        uint32_t fs=(v>=4)?syncsafe(fh+4):be32(fh+4);
        if(fs>ts-pos)break;
        uint8_t *d=(uint8_t*)malloc(fs);
        if(!d||fread(d,1,fs,f)!=fs){free(d);break;} pos+=fs;
        if(!memcmp(fh,ID3_FRAME_TITLE,4)&&fs>1)  printf("[ID3] 标题: %.*s\n",(int)(fs-1),d+1);
        if(!memcmp(fh,ID3_FRAME_ARTIST,4)&&fs>1) printf("[ID3] 艺术家: %.*s\n",(int)(fs-1),d+1);
        if(!memcmp(fh,ID3_FRAME_ALBUM,4)&&fs>1)  printf("[ID3] 专辑: %.*s\n",(int)(fs-1),d+1);
        if(!memcmp(fh,ID3_FRAME_COVER,4)&&fs>4){
            const char*m=(const char*)(d+1); size_t ml=strlen(m);
            const uint8_t*p=d+1+ml+1; uint8_t pt=*p++;
            while(*p&&p<d+fs)p++;
            p++;
            size_t isz=d+fs-p;
            printf("[ID3] 封面: MIME=%s, 类型=%s, 大小=%zu bytes\n", m,
                   pt==3?"封面(正面)":pt==4?"封面(背面)":"其他", isz);
        }
        free(d);
    }
    *off=10+ts+(ft?10:0);
}

static bool mp3_open(audio_decoder_t *iface, const char *path)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;

    FILE *f = fopen(path, "rb");
    if(!f) return false;

    long off = 0;
    parse_id3v2(f, &off);

    d->mp3 = new micro_mp3::Mp3Decoder();
    d->inbuf = (uint8_t *)heap_caps_malloc(MP3_INPUT_CHUNK_SIZE, MALLOC_CAP_DEFAULT);
    if(!d->mp3 || !d->inbuf){
        if(d->mp3){delete d->mp3; d->mp3=NULL;}
        if(d->inbuf){heap_caps_free(d->inbuf); d->inbuf=NULL;}
        fclose(f);
        return false;
    }

    fseek(f, off, SEEK_SET);
    d->file = f;
    d->eof  = false;
    d->info_done = false;
    d->in_off = 0;
    d->in_len = 0;

    {
        struct stat st;
        if(stat(path, &st)==0){
            printf("[音频] 文件大小=%ld bytes, ID3偏移=%ld → MP3数据=%ld bytes\n",
                   (long)st.st_size, off, (long)st.st_size - off);
        }
    }

    return true;
}

static bool mp3_decode_frame(audio_decoder_t *iface, int16_t *pcm, size_t *bytes)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;

    if(!d->file || !d->mp3 || !d->inbuf) return false;

    while(1){
        if(d->in_len == 0){
            d->in_len = fread(d->inbuf, 1, MP3_INPUT_CHUNK_SIZE, d->file);
            if(d->in_len == 0){ d->eof = true; return false; }
            d->in_off = 0;
        }

        const uint8_t *p = d->inbuf + d->in_off;
        size_t n = d->in_len, consumed = 0, samples = 0;
        auto res = d->mp3->decode(p, n, (uint8_t*)d->pcm_buf,
                                   sizeof(d->pcm_buf), consumed, samples);

        d->in_off += consumed;
        d->in_len -= consumed;
        if(d->in_len == 0) d->in_off = 0;

        if(res == micro_mp3::MP3_STREAM_INFO_READY ||
           res == micro_mp3::MP3_STREAM_INFO_CHANGED){
            if(!d->info_done){
                d->sample_rate = d->mp3->get_sample_rate();
                d->channels    = d->mp3->get_channels();
                printf("[音频] MP3: %" PRIu32 " Hz, %u ch, %" PRIu32 " kbps\n",
                       d->sample_rate, d->channels, d->mp3->get_bitrate());
                d->info_done = true;
            }
            continue;
        }
        if(res == micro_mp3::MP3_NEED_MORE_DATA){
            if(d->in_len == 0) continue;
            if(consumed == 0){
                if(d->in_off > 0){
                    memmove(d->inbuf, d->inbuf + d->in_off, d->in_len);
                    d->in_off = 0;
                }
                size_t nread = fread(d->inbuf + d->in_len, 1,
                                     MP3_INPUT_CHUNK_SIZE - d->in_len, d->file);
                if(nread == 0){ d->eof = true; return false; }
                d->in_len += nread;
            }
            continue;
        }
        if(res == micro_mp3::MP3_DECODE_ERROR) continue;
        if(res == micro_mp3::MP3_OUTPUT_BUFFER_TOO_SMALL) continue;
        if(res < 0){ d->eof = true; return false; }

        if(samples > 0){
            size_t total = samples * d->mp3->get_channels() * d->mp3->get_bytes_per_sample();
            memcpy(pcm, d->pcm_buf, total);
            *bytes = total;
            return true;
        }
    }
}

static bool mp3_is_eof(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->eof;
}

static void mp3_close(audio_decoder_t *iface)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    if(d->file){ fclose(d->file); d->file = NULL; }
    if(d->mp3){ delete d->mp3; d->mp3 = NULL; }
    if(d->inbuf){ heap_caps_free(d->inbuf); d->inbuf = NULL; }
    d->in_off = 0;
    d->in_len = 0;
    d->eof    = false;
}

static uint32_t mp3_get_sample_rate(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->sample_rate;
}

static uint8_t mp3_get_channels(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->channels;
}

audio_decoder_t *decoder_mp3_create(void)
{
    decoder_mp3_t *d = (decoder_mp3_t *)calloc(1, sizeof(decoder_mp3_t));
    if(!d) return NULL;

    d->iface.open            = mp3_open;
    d->iface.decode          = mp3_decode_frame;
    d->iface.is_eof          = mp3_is_eof;
    d->iface.close           = mp3_close;
    d->iface.get_sample_rate = mp3_get_sample_rate;
    d->iface.get_channels    = mp3_get_channels;

    return &d->iface;
}

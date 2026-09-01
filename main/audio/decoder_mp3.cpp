#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "micro_mp3/mp3_decoder.h"
#include "audio.h"

#define MP3_TAG "DEC_MP3"
#define MP3_INPUT_CHUNK_SIZE 2048
#define MP3_PCM_BUF_SAMPLES  (1152 * 2)

#define ID3_FRAME_TITLE   "TIT2"
#define ID3_FRAME_ARTIST  "TPE1"
#define ID3_FRAME_ALBUM   "TALB"
#define ID3_FRAME_COVER   "APIC"

#define COVER_MAX_SIZE    (512 * 1024)

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

    char     title[SONG_TITLE_MAX];
    char     artist[SONG_ARTIST_MAX];
    uint32_t file_size;

    uint8_t *cover_data;   /* APIC 图片字节 (PSRAM), 未移交时由 mp3_close 释放 */
    size_t   cover_size;
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

static void parse_id3v2(FILE *f, long *off,
                        char *title_out, size_t title_size,
                        char *artist_out, size_t artist_size,
                        uint8_t **cover_out, size_t *cover_size_out)
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
        if(!memcmp(fh,ID3_FRAME_TITLE,4)&&fs>1){
            size_t n=(size_t)(fs-1); if(n>=title_size)n=title_size-1;
            if(title_out){memcpy(title_out,d+1,n);title_out[n]='\0';}
            printf("[ID3] 标题: %.*s\n",(int)(fs-1),d+1);
        }
        if(!memcmp(fh,ID3_FRAME_ARTIST,4)&&fs>1){
            size_t n=(size_t)(fs-1); if(n>=artist_size)n=artist_size-1;
            if(artist_out){memcpy(artist_out,d+1,n);artist_out[n]='\0';}
            printf("[ID3] 艺术家: %.*s\n",(int)(fs-1),d+1);
        }
        if(!memcmp(fh,ID3_FRAME_ALBUM,4)&&fs>1)  printf("[ID3] 专辑: %.*s\n",(int)(fs-1),d+1);
        if(!memcmp(fh,ID3_FRAME_COVER,4)&&fs>4){
            const char*m=(const char*)(d+1); size_t ml=strlen(m);
            const uint8_t*p=d+1+ml+1; uint8_t pt=*p++;
            while(*p&&p<d+fs)p++;
            p++;
            size_t isz=d+fs-p;
            printf("[ID3] 封面: MIME=%s, 类型=%s, 大小=%zu bytes\n", m,
                   pt==3?"封面(正面)":pt==4?"封面(背面)":"其他", isz);
            if (cover_out && *cover_out == NULL && isz > 0 && isz <= COVER_MAX_SIZE) {
                uint8_t *buf = (uint8_t*)heap_caps_malloc(isz, MALLOC_CAP_SPIRAM);
                if (buf) {
                    memcpy(buf, p, isz);
                    *cover_out = buf;
                    if (cover_size_out) *cover_size_out = isz;
                    printf("[ID3] 封面字节已提取到 PSRAM: %zu bytes\n", isz);
                }
            }
        }
        free(d);
    }
    *off=10+ts+(ft?10:0);
}

static bool mp3_open(audio_decoder_t *iface, const char *path)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;

    /* 释放上次残留封面 (防止重复 open 泄漏) */
    if (d->cover_data) {
        heap_caps_free(d->cover_data);
        d->cover_data = NULL;
        d->cover_size = 0;
    }

    FILE *f = fopen(path, "rb");
    if(!f) return false;

    long off = 0;
    parse_id3v2(f, &off, d->title, sizeof(d->title), d->artist, sizeof(d->artist),
                &d->cover_data, &d->cover_size);

    d->mp3 = new micro_mp3::Mp3Decoder();
    d->inbuf = (uint8_t *)heap_caps_malloc(MP3_INPUT_CHUNK_SIZE, MALLOC_CAP_DEFAULT);
    if(!d->mp3 || !d->inbuf){
        if(d->mp3){delete d->mp3; d->mp3=NULL;}
        if(d->inbuf){heap_caps_free(d->inbuf); d->inbuf=NULL;}
        if(d->cover_data){heap_caps_free(d->cover_data); d->cover_data=NULL; d->cover_size=0;}
        fclose(f);
        return false;
    }

    fseek(f, off, SEEK_SET);
    d->file = f;
    d->eof  = false;
    d->info_done = false;
    d->in_off = 0;
    d->in_len = 0;
    d->file_size = 0;

    {
        struct stat st;
        if(stat(path, &st)==0){
            d->file_size = (uint32_t)st.st_size;
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
    if(d->cover_data){ heap_caps_free(d->cover_data); d->cover_data = NULL; d->cover_size = 0; }
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

/* MP3 位深固定 16bit */
static uint8_t mp3_get_bits(audio_decoder_t *iface)
{
    return 16;
}

static uint32_t mp3_get_bitrate(audio_decoder_t *iface)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    return d->mp3 ? d->mp3->get_bitrate() : 0;
}

static uint32_t mp3_get_file_size(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->file_size;
}

static uint32_t mp3_get_position(audio_decoder_t *iface)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    if (!d->file) return 0;
    long pos = ftell(d->file);
    return pos > 0 ? (uint32_t)pos : 0;
}

static bool mp3_seek(audio_decoder_t *iface, uint32_t byte_offset)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    if(!d->file || !d->mp3) return false;
    if(byte_offset > d->file_size) byte_offset = d->file_size;
    if(fseek(d->file, (long)byte_offset, SEEK_SET) != 0) return false;
    d->mp3->reset();
    d->in_off = 0;
    d->in_len = 0;
    d->eof    = false;
    d->info_done = false;
    printf("[音频] SEEK -> %" PRIu32 " bytes\n", byte_offset);
    return true;
}

static const char *mp3_get_title(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->title;
}

static const char *mp3_get_artist(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->artist;
}

static const uint8_t *mp3_get_cover_data(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->cover_data;
}

static size_t mp3_get_cover_size(audio_decoder_t *iface)
{
    return ((decoder_mp3_t *)iface)->cover_size;
}

/* 所有权移交给封面模块后, 标记解码器不再持有, 避免 close 时重复释放 */
static void mp3_take_cover(audio_decoder_t *iface)
{
    decoder_mp3_t *d = (decoder_mp3_t *)iface;
    d->cover_data = NULL;
    d->cover_size = 0;
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
    d->iface.get_bits        = mp3_get_bits;
    d->iface.get_bitrate     = mp3_get_bitrate;
    d->iface.get_file_size   = mp3_get_file_size;
    d->iface.get_position    = mp3_get_position;
    d->iface.seek            = mp3_seek;
    d->iface.get_title       = mp3_get_title;
    d->iface.get_artist      = mp3_get_artist;
    d->iface.get_cover_data  = mp3_get_cover_data;
    d->iface.get_cover_size  = mp3_get_cover_size;
    d->iface.take_cover      = mp3_take_cover;

    return &d->iface;
}

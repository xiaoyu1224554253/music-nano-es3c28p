#ifndef __DECODER_H__
#define __DECODER_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_decoder_s {
    bool      (*open)(struct audio_decoder_s *self, const char *path);
    bool      (*decode)(struct audio_decoder_s *self, int16_t *pcm, size_t *bytes);
    bool      (*is_eof)(struct audio_decoder_s *self);
    void      (*close)(struct audio_decoder_s *self);
    uint32_t  (*get_sample_rate)(struct audio_decoder_s *self);
    uint8_t   (*get_channels)(struct audio_decoder_s *self);
    uint32_t  (*get_bitrate)(struct audio_decoder_s *self);
    uint32_t  (*get_file_size)(struct audio_decoder_s *self);
    uint32_t  (*get_position)(struct audio_decoder_s *self);
    bool      (*seek)(struct audio_decoder_s *self, uint32_t byte_offset);
    const char *(*get_title)(struct audio_decoder_s *self);
    const char *(*get_artist)(struct audio_decoder_s *self);
    const uint8_t *(*get_cover_data)(struct audio_decoder_s *self);
    size_t         (*get_cover_size)(struct audio_decoder_s *self);
    void           (*take_cover)(struct audio_decoder_s *self);
} audio_decoder_t;

audio_decoder_t *decoder_mp3_create(void);

#ifdef __cplusplus
}
#endif

#endif

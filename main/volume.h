#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOLUME_MIN      0
#define VOLUME_MAX      127

int32_t volume_get(void);
void    volume_set(int32_t v);
void    volume_inc(int32_t delta);

void volume_load_from_nvs(void);
void volume_save_to_nvs(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void last_song_save(const char *path);
bool last_song_load(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

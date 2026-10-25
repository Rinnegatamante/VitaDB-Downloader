#ifndef _PREVIEW_CACHE_H
#define _PREVIEW_CACHE_H

#include <vitaGL.h>

bool preview_cache_init();
void preview_cache_prefetch(const char *path);
bool preview_cache_get(const char *path, GLuint *texture, int *width, int *height);
void preview_cache_shutdown();

#endif

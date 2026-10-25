/*
 * This file is part of VitaDB Downloader
 * Copyright 2026 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>
#include <vitaGL.h>
#include "stb_image.h"
#include "preview_cache.h"

#define PREVIEW_CACHE_ENTRIES 40
#define PREVIEW_PATH_SIZE 256

#define PREVIEW_PENDING_RGBA_BUDGET (4 * 1024 * 1024)
#define PREVIEW_GPU_RGBA_BUDGET (8 * 1024 * 1024)

#define PREVIEW_COMPRESSED_LIMIT (256 * 1024)

// Only the worker calls this. Compressed bytes are temporary and never cached.
static uint8_t *decode_preview_one_read(const char *path, int *w, int *h) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) { // This should never happen...
		return stbi_load(path, w, h, nullptr, 4);
	}

	// When possible, prefer stbi_load_from_memory to avoid small I/O calls.
	SceOff length = sceIoLseek(fd, 0, SCE_SEEK_END);
	if (length <= 0 || length > PREVIEW_COMPRESSED_LIMIT || sceIoLseek(fd, 0, SCE_SEEK_SET) < 0) {
		sceIoClose(fd);
		return stbi_load(path, w, h, nullptr, 4);
	}
	uint8_t *compressed = (uint8_t *)malloc((size_t)length);
	if (!compressed) {
		sceIoClose(fd);
		return stbi_load(path, w, h, nullptr, 4);
	}
	size_t received = 0;
	while (received < (size_t)length) {
		int n = sceIoRead(fd, compressed + received, (size_t)length - received);
		if (n <= 0) {
			break;
		}
		received += (size_t)n;
	}
	sceIoClose(fd);
	uint8_t *pixels = nullptr;
	if (received == (size_t)length)
		pixels = stbi_load_from_memory(compressed, (int)length, w, h, nullptr, 4);
	free(compressed);
	return pixels;
}

enum {
	PREVIEW_EMPTY,
	PREVIEW_QUEUED,
	PREVIEW_DECODING,
	PREVIEW_DECODED,
	PREVIEW_UPLOADING,
	PREVIEW_UPLOADED,
	PREVIEW_FAILED
};

struct PreviewEntry {
	char path[PREVIEW_PATH_SIZE];
	uint8_t *pixels;
	GLuint texture;
	int width, height;
	uint64_t last_use;
	uint8_t state;
};

// Icons cache
static PreviewEntry preview_entries[PREVIEW_CACHE_ENTRIES] = {};
static SceUID preview_mutex = -1;
static SceUID preview_thread = -1;
static std::atomic<bool> preview_stop(false);
static uint64_t preview_tick = 0;
static char preview_active[PREVIEW_PATH_SIZE] = {};
static char preview_displayed[PREVIEW_PATH_SIZE] = {};

static PreviewEntry *find_preview(const char *path) {
	for (int i = 0; i < PREVIEW_CACHE_ENTRIES; i++) {
		if (preview_entries[i].state != PREVIEW_EMPTY && !strcmp(preview_entries[i].path, path)) {
			return &preview_entries[i];
		}
	}
	return nullptr;
}

static size_t preview_bytes(const PreviewEntry *entry) {
	return entry->width > 0 && entry->height > 0 ?
		(size_t)entry->width * (size_t)entry->height * 4 : 0;
}

static PreviewEntry *queue_preview(const char *path) {
	PreviewEntry *entry = find_preview(path);
	if (entry) {
		entry->last_use = ++preview_tick;
		return entry;
	}
	PreviewEntry *candidate = nullptr;
	for (int i = 0; i < PREVIEW_CACHE_ENTRIES; i++) {
		PreviewEntry *item = &preview_entries[i];
		if (item->state == PREVIEW_EMPTY) {
			candidate = item;
			break;
		}
		if (item->state == PREVIEW_DECODING || item->state == PREVIEW_UPLOADING ||
			!strcmp(item->path, preview_active) || !strcmp(item->path, preview_displayed)) {
			continue;
		}
		if (!candidate || item->last_use < candidate->last_use) {
			candidate = item;
		}
	}
	if (!candidate) {
		return nullptr;
	}
	if (candidate->texture) {
		glDeleteTextures(1, &candidate->texture);
	}
	if (candidate->pixels) {
		stbi_image_free(candidate->pixels);
	}
	memset(candidate, 0, sizeof(*candidate));
	snprintf(candidate->path, sizeof(candidate->path), "%s", path);
	candidate->last_use = ++preview_tick;
	candidate->state = PREVIEW_QUEUED;
	return candidate;
}

static int preview_worker(unsigned int args, void *arg) {
	while (!preview_stop.load(std::memory_order_acquire)) {
		PreviewEntry *next = nullptr;
		char path[PREVIEW_PATH_SIZE];
		sceKernelLockMutex(preview_mutex, 1, nullptr);
		for (int i = 0; i < PREVIEW_CACHE_ENTRIES; i++) {
			PreviewEntry *item = &preview_entries[i];
			if (item->state != PREVIEW_QUEUED) {
				continue;
			}
			bool active = !strcmp(item->path, preview_active);
			bool next_active = next && !strcmp(next->path, preview_active);
			if (!next || (active && !next_active) ||
				(active == next_active && item->last_use > next->last_use)) {
				next = item;
			}
		}
		if (next) {
			next->state = PREVIEW_DECODING;
			snprintf(path, sizeof(path), "%s", next->path);
		}
		sceKernelUnlockMutex(preview_mutex, 1);
		if (!next) {
			sceKernelDelayThread(8000);
			continue;
		}

		int w = 0, h = 0;
		uint8_t *pixels = decode_preview_one_read(path, &w, &h);
		if (pixels && (w <= 0 || h <= 0 || w > 1024 || h > 1024)) {
			stbi_image_free(pixels);
			pixels = nullptr;
		}
		sceKernelLockMutex(preview_mutex, 1, nullptr);
		if (next->state == PREVIEW_DECODING) {
			size_t occupied = 0;
			for (int i = 0; i < PREVIEW_CACHE_ENTRIES; i++) {
				if (preview_entries[i].state == PREVIEW_DECODED)
					occupied += preview_bytes(&preview_entries[i]);
			}
			size_t incoming = pixels ? (size_t)w * (size_t)h * 4 : 0;
			while (pixels && occupied + incoming > PREVIEW_PENDING_RGBA_BUDGET) {
				PreviewEntry *oldest = nullptr;
				for (int i = 0; i < PREVIEW_CACHE_ENTRIES; i++) {
					PreviewEntry *item = &preview_entries[i];
					if (item == next || item->state != PREVIEW_DECODED ||
					    !strcmp(item->path, preview_active) ||
					    !strcmp(item->path, preview_displayed))
						continue;
					if (!oldest || item->last_use < oldest->last_use)
						oldest = item;
				}
				if (!oldest)
					break;
				occupied -= preview_bytes(oldest);
				stbi_image_free(oldest->pixels);
				memset(oldest, 0, sizeof(*oldest));
			}
			if (pixels && occupied + incoming > PREVIEW_PENDING_RGBA_BUDGET) {
				stbi_image_free(pixels);
				pixels = nullptr;
			}
			next->width = pixels ? w : 0;
			next->height = pixels ? h : 0;
			next->pixels = pixels;
			next->state = pixels ? PREVIEW_DECODED : PREVIEW_FAILED;
		} else if (pixels) {
			stbi_image_free(pixels);
		}
		sceKernelUnlockMutex(preview_mutex, 1);
	}
	return sceKernelExitDeleteThread(0);
}

bool preview_cache_init() {
	if (preview_mutex >= 0) {
		return true;
	}
	preview_mutex = sceKernelCreateMutex("VitaDB Preview Cacher", 0, 0, nullptr);
	if (preview_mutex < 0) {
		return false;
	}
	preview_stop.store(false, std::memory_order_release);
	preview_thread = sceKernelCreateThread("VitaDB Preview Loader", preview_worker, 0x10000110, 0x40000, 0, 0, nullptr);
	if (preview_thread < 0) {
		sceKernelDeleteMutex(preview_mutex);
		preview_mutex = -1;
		return false;
	}
	if (sceKernelStartThread(preview_thread, 0, nullptr) < 0) {
		sceKernelDeleteThread(preview_thread);
		preview_thread = -1;
		sceKernelDeleteMutex(preview_mutex);
		preview_mutex = -1;
		return false;
	}
	return true;
}

void preview_cache_prefetch(const char *path) {
	if (!path || !path[0] || strlen(path) >= PREVIEW_PATH_SIZE || !preview_cache_init()) {
		return;
	}
	sceKernelLockMutex(preview_mutex, 1, nullptr);
	queue_preview(path);
	sceKernelUnlockMutex(preview_mutex, 1);
}

bool preview_cache_get(const char *path, GLuint *texture, int *width, int *height) {
	if (!path || !path[0] || strlen(path) >= PREVIEW_PATH_SIZE || !preview_cache_init()) {
		return false;
	}
	sceKernelLockMutex(preview_mutex, 1, nullptr);
	snprintf(preview_active, sizeof(preview_active), "%s", path);
	PreviewEntry *entry = queue_preview(path);
	if (!entry) {
		sceKernelUnlockMutex(preview_mutex, 1);
		return false;
	}
	if (entry->state == PREVIEW_UPLOADED) {
		*texture = entry->texture;
		*width = entry->width;
		*height = entry->height;
		snprintf(preview_displayed, sizeof(preview_displayed), "%s", path);
		sceKernelUnlockMutex(preview_mutex, 1);
		return true;
	}
	if (entry->state == PREVIEW_FAILED) {
		*texture = 0;
		*width = *height = 0;
		snprintf(preview_displayed, sizeof(preview_displayed), "%s", path);
		sceKernelUnlockMutex(preview_mutex, 1);
		return true;
	}
	if (entry->state != PREVIEW_DECODED) {
		sceKernelUnlockMutex(preview_mutex, 1);
		return false;
	}
	uint8_t *pixels = entry->pixels;
	int w = entry->width, h = entry->height;
	size_t required = preview_bytes(entry);
	size_t occupied = 0;
	for (int i = 0; i < PREVIEW_CACHE_ENTRIES; i++) {
		if (preview_entries[i].texture)
			occupied += preview_bytes(&preview_entries[i]);
	}
	while (occupied + required > PREVIEW_GPU_RGBA_BUDGET) {
		PreviewEntry *oldest = nullptr;
		for (int i = 0; i < PREVIEW_CACHE_ENTRIES; i++) {
			PreviewEntry *item = &preview_entries[i];
			if (item == entry || !item->texture || item->state != PREVIEW_UPLOADED ||
			    !strcmp(item->path, preview_active) ||
			    !strcmp(item->path, preview_displayed))
				continue;
			if (!oldest || item->last_use < oldest->last_use)
				oldest = item;
		}
		if (!oldest)
			break;
		occupied -= preview_bytes(oldest);
		glDeleteTextures(1, &oldest->texture);
		memset(oldest, 0, sizeof(*oldest));
	}
	if (required > PREVIEW_GPU_RGBA_BUDGET ||
	    occupied + required > PREVIEW_GPU_RGBA_BUDGET) {
		entry->pixels = nullptr;
		entry->state = PREVIEW_FAILED;
		sceKernelUnlockMutex(preview_mutex, 1);
		stbi_image_free(pixels);
		return false;
	}
	entry->pixels = nullptr;
	entry->state = PREVIEW_UPLOADING;
	sceKernelUnlockMutex(preview_mutex, 1);

	GLuint tex;
	glGenTextures(1, &tex);
	glTextureImage2D(tex, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	stbi_image_free(pixels);

	sceKernelLockMutex(preview_mutex, 1, nullptr);
	entry->texture = tex;
	entry->state = PREVIEW_UPLOADED;
	snprintf(preview_displayed, sizeof(preview_displayed), "%s", path);
	sceKernelUnlockMutex(preview_mutex, 1);
	*texture = tex;
	*width = w;
	*height = h;
	return true;
}

void preview_cache_shutdown() {
	if (preview_mutex < 0) {
		return;
	}
	preview_stop.store(true, std::memory_order_release);
	if (preview_thread >= 0) {
		sceKernelWaitThreadEnd(preview_thread, nullptr, nullptr);
		preview_thread = -1;
	}
	for (int i = 0; i < PREVIEW_CACHE_ENTRIES; i++) {
		if (preview_entries[i].texture) {
			glDeleteTextures(1, &preview_entries[i].texture);
		}
		if (preview_entries[i].pixels) {
			stbi_image_free(preview_entries[i].pixels);
		}
	}
	sceKernelDeleteMutex(preview_mutex);
	preview_mutex = -1;
}

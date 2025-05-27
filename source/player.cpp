/*
 * This file is part of VitaDB Downloader
 * Copyright 2025 Rinnegatamante
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <stdio.h>
#include <malloc.h>

#include "network.h"

#define VIDEO_BUFFERS_NUM (5)
#define FB_ALIGNMENT 0x40000
#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

//#define DEBUG_PLAYER // Uncomment this to enable video player debugging

extern int video_decoder_idx;

enum {
	PLAYER_INACTIVE,
	PLAYER_ACTIVE,
	PLAYER_STOP,
};

enum {
	PLAYER_UNPAUSED,
	PLAYER_PAUSED
};

SceAvPlayerHandle movie_player;

static int player_state = PLAYER_INACTIVE;
static int player_pause_state = PLAYER_UNPAUSED;

GLuint movie_frame[VIDEO_BUFFERS_NUM];
uint8_t movie_frame_idx = 0;
SceGxmTexture *movie_tex[VIDEO_BUFFERS_NUM];
bool first_frame = true;

static int audio_new;
static int audio_port;
static int audio_len;
static int audio_freq;
static int audio_mode;
static SceUID audio_thid;

static bool is_local = true;

void *mem_alloc(void *p, uint32_t align, uint32_t size) {
	return memalign(align, size);
}

void mem_free(void *p, void *ptr) {
	free(ptr);
}

void *gpu_alloc(void *p, uint32_t align, uint32_t size) {
	if (align < FB_ALIGNMENT) {
		align = FB_ALIGNMENT;
	}
	size = ALIGN_MEM(size, align);
	return vglAlloc(size, VGL_MEM_SLOW);
}

void gpu_free(void *p, void *ptr) {
	glFinish();
	vglFree(ptr);
}

int video_audio_thread(SceSize args, void *argp) {
	SceAvPlayerFrameInfo frame;
	sceClibMemset(&frame, 0, sizeof(SceAvPlayerFrameInfo));

	while (player_state == PLAYER_ACTIVE && sceAvPlayerIsActive(movie_player)) {
		if (sceAvPlayerGetAudioData(movie_player, &frame)) {
			sceAudioOutSetConfig(audio_port, 1024, frame.details.audio.sampleRate, frame.details.audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
			sceAudioOutOutput(audio_port, frame.pData);
		} else {
			sceKernelDelayThread(1000);
		}
	}

	return sceKernelExitDeleteThread(0);
}

void video_audio_init(void) {
	// Check if we have an already available audio port
	audio_port = -1;
	for (int i = 0; i < 8; i++) {
		if (sceAudioOutGetConfig(i, SCE_AUDIO_OUT_CONFIG_TYPE_LEN) >= 0) {
			audio_port = i;
			break;
		}
	}

	// Configure the audio port (either new or old)
	if (audio_port == -1) {
		audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, 1024, 44100, SCE_AUDIO_OUT_MODE_STEREO);
		audio_new = 1;
	} else {
		audio_len = sceAudioOutGetConfig(audio_port, SCE_AUDIO_OUT_CONFIG_TYPE_LEN);
		audio_freq = sceAudioOutGetConfig(audio_port, SCE_AUDIO_OUT_CONFIG_TYPE_FREQ);
		audio_mode = sceAudioOutGetConfig(audio_port, SCE_AUDIO_OUT_CONFIG_TYPE_MODE);
		audio_new = 0;
	}
}

void video_audio_shutdown(void) {
	// Release or set back old config on used audio port
	if (audio_new) {
		sceAudioOutReleasePort(audio_port);
	} else {
		sceAudioOutSetConfig(audio_port, audio_len, audio_freq, audio_mode);
	}
}

int video_stream_start(void *argp, const char *url) {
	return 0;
}

int video_stream_stop(void *argp) {
	return 0;
}

int video_stream_read(void *argp, uint8_t *buffer, uint64_t pos, uint32_t len) {
#ifdef DEBUG_PLAYER
	sceClibPrintf("player.cpp: Called video_stream_read with pos: 0x%llX size: %u\n", pos, len);
#endif
	
	// Wait until downloader catches up with the decoder
	while (pos + len > downloaded_bytes) {
		// If we reached end of video, we just throw at the decoder last bytes left
		if (downloaded_bytes == total_bytes) {
			len = downloaded_bytes - pos;
			break;
		}

#ifdef DEBUG_PLAYER
		sceClibPrintf("player:cpp video_stream_read pos: 0x%llX size: %u with total: %llX/%llX\n", pos, len, downloaded_bytes, total_bytes);
#endif		
		sceKernelDelayThread(1000);
	}
	
	// Properly transfering downloaded data to decoder (FIXME: sceClibMemcpy here causes some random Prefetch Abort exceptions raising)
	int buffer_id = ((pos + len) / VIDEO_DECODER_BUFFER_SIZE) % 2;
	uint64_t normalized_pos = pos % VIDEO_DECODER_BUFFER_SIZE;
	if (buffer_id != video_decoder_idx) { // Buffer fulled, buffer swap required
		memcpy(buffer, &generic_mem_buffer[video_decoder_idx * VIDEO_DECODER_BUFFER_SIZE + normalized_pos], VIDEO_DECODER_BUFFER_SIZE - normalized_pos);
		video_decoder_idx = (video_decoder_idx + 1) % 2;
		memcpy(buffer + VIDEO_DECODER_BUFFER_SIZE - normalized_pos, &generic_mem_buffer[video_decoder_idx * VIDEO_DECODER_BUFFER_SIZE], len - (VIDEO_DECODER_BUFFER_SIZE - normalized_pos));
	} else
		memcpy(buffer, &generic_mem_buffer[video_decoder_idx * VIDEO_DECODER_BUFFER_SIZE + normalized_pos], len);
	
	return len;
}

int video_stream_size(void *argp) {
	// Wait until the downloader properly parsed response header for real video file size
	while (total_bytes == 0xFFFFFFFF) {
		sceKernelDelayThread(1000);
	}

	return total_bytes;
}

void video_close() {
	if (player_state == PLAYER_ACTIVE) {
		sceAvPlayerStop(movie_player);
		if (!is_local) {
			sceKernelWaitThreadEnd(audio_thid, NULL, NULL);
			video_audio_shutdown();
		}
		sceAvPlayerClose(movie_player);
		player_state = PLAYER_INACTIVE;
		glDeleteTextures(VIDEO_BUFFERS_NUM, movie_frame);
	}
}

void video_open(const char *path) {
	first_frame = true;
	glGenTextures(VIDEO_BUFFERS_NUM, movie_frame);
	for (int i = 0; i < VIDEO_BUFFERS_NUM; i++) {
		glBindTexture(GL_TEXTURE_2D, movie_frame[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		movie_tex[i] = vglGetGxmTexture(GL_TEXTURE_2D);
		vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
	}
	
	// Check if the supplied path is a remote video
	is_local = strncmp(path, "http", 4);
	if (!is_local) {
		stream_video((char *)path);
	}
	
	SceAvPlayerInitData playerInit;
	memset(&playerInit, 0, sizeof(SceAvPlayerInitData));

	playerInit.memoryReplacement.allocate = mem_alloc;
	playerInit.memoryReplacement.deallocate = mem_free;
	playerInit.memoryReplacement.allocateTexture = gpu_alloc;
	playerInit.memoryReplacement.deallocateTexture = gpu_free;

	playerInit.basePriority = 0xA0;
	playerInit.numOutputVideoFrameBuffers = VIDEO_BUFFERS_NUM;
	playerInit.autoStart = GL_TRUE;
#if 0
	playerInit.debugLevel = 3;
#endif

	// Set callbacks for video streaming
	if (!is_local) {
		playerInit.fileReplacement.objectPointer = NULL;
		playerInit.fileReplacement.open = video_stream_start;
		playerInit.fileReplacement.close = video_stream_stop;
		playerInit.fileReplacement.readOffset = video_stream_read;
		playerInit.fileReplacement.size = video_stream_size;
		
		movie_player = sceAvPlayerInit(&playerInit);
		sceAvPlayerAddSource(movie_player, "remote_stream.mp4"); // sceAvPlayer needs the source to end with ".mp4" for some reasons...
		
		video_audio_init();
		audio_thid = sceKernelCreateThread("video_audio_thread", video_audio_thread, 0x10000100 - 10, 0x4000, 0, 0, NULL);
		sceKernelStartThread(audio_thid, 0, NULL);
	} else {
		movie_player = sceAvPlayerInit(&playerInit);
		sceAvPlayerAddSource(movie_player, path);
		sceAvPlayerSetLooping(movie_player, 1);
	}
	
	player_state = PLAYER_ACTIVE;
}

GLuint video_get_frame(int *width, int *height) {
	if (player_state == PLAYER_ACTIVE) {
		if (sceAvPlayerIsActive(movie_player)) {
			SceAvPlayerFrameInfo frame;
			if (sceAvPlayerGetVideoData(movie_player, &frame)) {
				movie_frame_idx = (movie_frame_idx + 1) % VIDEO_BUFFERS_NUM;
				sceGxmTextureInitLinear(
					movie_tex[movie_frame_idx],
					frame.pData,
					SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
					frame.details.video.width,
					frame.details.video.height, 0);
				*width = frame.details.video.width;
				*height = frame.details.video.height;
				sceGxmTextureSetMinFilter(movie_tex[movie_frame_idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
				sceGxmTextureSetMagFilter(movie_tex[movie_frame_idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
				first_frame = false;
			}
			return first_frame ? 0xDEADBEEF : movie_frame[movie_frame_idx];
		}
	}
	
	return 0xDEADBEEF;
}

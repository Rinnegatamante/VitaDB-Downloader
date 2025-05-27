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
 
#include <iostream>
#include <string>
#include <locale>
#include <curl/curl.h>
#include <stdio.h>
#include <string>
#include <vitasdk.h>
#include <vitaGL.h>
#include "dialogs.h"
#include "network.h"

//#define DEBUG_NET // Uncomment this to enable downloader debugging

volatile char generic_url[512];
static CURL *curl_handle = NULL;
volatile uint64_t total_bytes = 0xFFFFFFFF;
volatile uint64_t downloaded_bytes = 0;
volatile uint8_t downloader_pass = 1;
volatile bool is_cancelable = false;
volatile bool is_canceled = false;
uint8_t *generic_mem_buffer = nullptr;
static SceUID fh;
char *bytes_string;
extern int SCE_CTRL_CANCEL;

volatile uint64_t video_buffer_bytes = 0;
int video_decoder_idx = 0;
int video_downloader_idx = 0;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
	if (is_cancelable) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_CANCEL) {
			is_canceled = true;
			return 0;
		}
	}
	if (total_bytes > MEM_BUFFER_SIZE || fh >= 0) {
		if (fh < 0)
			fh = sceIoOpen(TEMP_DOWNLOAD_NAME, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		sceIoWrite(fh, ptr, nmemb);
	} else {
		uint8_t *dst = &generic_mem_buffer[downloaded_bytes];
		sceClibMemcpy(dst, ptr, nmemb);
	}
	downloaded_bytes += nmemb;
	if (total_bytes < downloaded_bytes)
		total_bytes = downloaded_bytes;
	return nmemb;
}

static size_t write_video_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
	if (is_cancelable) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_CANCEL) {
			is_canceled = true;
			return 0;
		}
	}
#ifdef DEBUG_NET
	sceClibPrintf("network.cpp: write_video_cb called while video_buffer_bytes is 0x%llX and downloaded_bytes is 0x%llX\n", video_buffer_bytes, downloaded_bytes);
#endif
	if (video_buffer_bytes + nmemb > VIDEO_DECODER_BUFFER_SIZE) { // Buffer fulled, buffer swap required
		sceClibMemcpy(&generic_mem_buffer[video_downloader_idx * VIDEO_DECODER_BUFFER_SIZE + video_buffer_bytes], ptr, VIDEO_DECODER_BUFFER_SIZE - video_buffer_bytes);
		video_downloader_idx = (video_downloader_idx + 1) % 2;
		
		// We wait until decoder has completely consumed at least a buffer prior to keep downloading new data
		while (video_downloader_idx == video_decoder_idx) {
			// If a request to stop current curl handle is found, we just intentionally cause curl to error out
			if (is_cancelable) {
				SceCtrlData pad;
				sceCtrlPeekBufferPositive(0, &pad, 1);
				if (pad.buttons & SCE_CTRL_CANCEL) {
					is_canceled = true;
					return 0;
				}
			}
			sceKernelDelayThread(1000);
		}
		
		sceClibMemcpy(&generic_mem_buffer[video_downloader_idx * VIDEO_DECODER_BUFFER_SIZE], (uint8_t *)ptr + (VIDEO_DECODER_BUFFER_SIZE - video_buffer_bytes), nmemb - (VIDEO_DECODER_BUFFER_SIZE - video_buffer_bytes));
		video_buffer_bytes = nmemb - (VIDEO_DECODER_BUFFER_SIZE - video_buffer_bytes);
	} else {
		sceClibMemcpy(&generic_mem_buffer[video_downloader_idx * VIDEO_DECODER_BUFFER_SIZE + video_buffer_bytes], ptr, nmemb);
		video_buffer_bytes += nmemb;
	}
	downloaded_bytes += nmemb;
	return nmemb;
}

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
	char *ptr = strcasestr(buffer, "Content-Length");
	if (ptr != NULL) {
		sscanf(ptr, "Content-Length: %llu", &total_bytes);
#ifdef DEBUG_NET
		sceClibPrintf("network.cpp: Detected total length of %llu bytes.\n", total_bytes);
#endif
	}
	return nitems;
}

static size_t header_dummy_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
	return nitems;
}

static void startDownload(const char *url) {
	curl_easy_reset(curl_handle);
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.36");
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
	curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, bytes_string); // Dummy
	curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, downloaded_bytes ? header_dummy_cb : header_cb);
	curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, bytes_string); // Dummy
	curl_easy_setopt(curl_handle, CURLOPT_RESUME_FROM, downloaded_bytes);
	curl_easy_setopt(curl_handle, CURLOPT_BUFFERSIZE, 524288);
	struct curl_slist *headerchunk = NULL;
	headerchunk = curl_slist_append(headerchunk, "Accept: */*");
	headerchunk = curl_slist_append(headerchunk, "Content-Type: application/json");
	headerchunk = curl_slist_append(headerchunk, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.36");
	headerchunk = curl_slist_append(headerchunk, "Content-Length: 0");
	curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headerchunk);
	curl_easy_perform(curl_handle);
}

static void startStream(const char *url) {
	curl_easy_reset(curl_handle);
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.36");
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
	curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_video_cb);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, bytes_string); // Dummy
	curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, downloaded_bytes ? header_dummy_cb : header_cb);
	curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, bytes_string); // Dummy
	curl_easy_setopt(curl_handle, CURLOPT_RESUME_FROM, downloaded_bytes);
	curl_easy_setopt(curl_handle, CURLOPT_BUFFERSIZE, 524288);
	struct curl_slist *headerchunk = NULL;
	headerchunk = curl_slist_append(headerchunk, "Accept: */*");
	headerchunk = curl_slist_append(headerchunk, "Content-Type: application/json");
	headerchunk = curl_slist_append(headerchunk, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.36");
	headerchunk = curl_slist_append(headerchunk, "Content-Length: 0");
	curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headerchunk);
	curl_easy_perform(curl_handle);
}

int appListThread(unsigned int args, void *arg) {
	curl_handle = curl_easy_init();
	downloader_pass = 1;
	downloaded_bytes = 0;
	fh = -1;

	SceIoStat stat;
	sceIoGetstat("ux0:data/VitaDB/apps.json", &stat);
	total_bytes = stat.st_size;

	startDownload("https://www.rinnegatamante.eu/vitadb/list_hbs_json.php");

	if (downloaded_bytes > 12 * 1024) {
		fh = sceIoOpen("ux0:data/VitaDB/apps.json", SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		sceIoWrite(fh, generic_mem_buffer, downloaded_bytes);
		sceIoClose(fh);
	}
	downloaded_bytes = total_bytes;
	curl_easy_cleanup(curl_handle);
	return sceKernelExitDeleteThread(0);
}

int appPspListThread(unsigned int args, void *arg) {
	curl_handle = curl_easy_init();
	downloader_pass = 1;
	downloaded_bytes = 0;
	fh = -1;

	SceIoStat stat;
	sceIoGetstat("ux0:data/VitaDB/psp_apps.json", &stat);
	total_bytes = stat.st_size;

	startDownload("https://www.rinnegatamante.eu/vitadb/list_psp_hbs_json.php");

	if (downloaded_bytes > 12 * 1024) {
		fh = sceIoOpen("ux0:data/VitaDB/psp_apps.json", SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		sceIoWrite(fh, generic_mem_buffer, downloaded_bytes);
		sceIoClose(fh);
	}
	downloaded_bytes = total_bytes;
	curl_easy_cleanup(curl_handle);
	return sceKernelExitDeleteThread(0);
}

int downloadMemThread(unsigned int args, void *arg) {
	curl_handle = curl_easy_init();
	//printf("downloading %s\n", generic_url);
	char *url = (char *)generic_url;
	char *space = strstr(url, " ");
	char *s = url;
	char final_url[512] = "";
	fh = -1;
	while (space) {
		space[0] = 0;
		sprintf(final_url, "%s%s%%20", final_url, s);
		space[0] = ' ';
		s = space + 1;
		space = strstr(s, " ");
	}
	sprintf(final_url, "%s%s", final_url, s);
	//printf("starting download of %s\n", final_url);
	downloader_pass = 1;
	downloaded_bytes = 0;
	total_bytes = 20;
	startDownload(final_url);
	while (downloaded_bytes < total_bytes) {
		startDownload(final_url);
	}
	downloaded_bytes = total_bytes;
	generic_mem_buffer[downloaded_bytes] = 0;
	curl_easy_cleanup(curl_handle);
	return sceKernelExitDeleteThread(0);
}

int streamMemThread(unsigned int args, void *arg) {
	curl_handle = curl_easy_init();
	//printf("streaming %s\n", generic_url);
	char *url = (char *)generic_url;
	char *space = strstr(url, " ");
	char *s = url;
	char final_url[512] = "";
	fh = -1;
	while (space) {
		space[0] = 0;
		sprintf(final_url, "%s%s%%20", final_url, s);
		space[0] = ' ';
		s = space + 1;
		space = strstr(s, " ");
	}
	sprintf(final_url, "%s%s", final_url, s);
	//printf("starting stream of %s\n", final_url);
	downloader_pass = 1;
	downloaded_bytes = 0;
	video_buffer_bytes = 0;
	video_downloader_idx = 0;
	video_decoder_idx = 0;
	total_bytes = 20;
	startStream(final_url);
	while (downloaded_bytes < total_bytes) {
		if (is_canceled) {
			goto ABORT_DOWNLOAD;
		}
		startStream(final_url);
	}
ABORT_DOWNLOAD:
	downloaded_bytes = total_bytes;
	generic_mem_buffer[downloaded_bytes] = 0;
	curl_easy_cleanup(curl_handle);
	return sceKernelExitDeleteThread(0);
}

int downloadThread(unsigned int args, void *arg) {
	curl_handle = curl_easy_init();
	//printf("downloading %s\n", generic_url);
	char *url = (char *)generic_url;
	char *space = strstr(url, " ");
	char *s = url;
	char final_url[512] = "";
	fh = -1;
	while (space) {
		space[0] = 0;
		sprintf(final_url, "%s%s%%20", final_url, s);
		space[0] = ' ';
		s = space + 1;
		space = strstr(s, " ");
	}
	sprintf(final_url, "%s%s", final_url, s);
	//printf("starting download of %s\n", final_url);
	downloader_pass = 1;
	downloaded_bytes = 0;
	total_bytes = 180;
	startDownload(final_url);
	while (downloaded_bytes < total_bytes) {
		if (is_cancelable && is_canceled) {
			goto ABORT_DOWNLOAD;
		}
		startDownload(final_url);
	}
	if (downloaded_bytes > 180 && total_bytes <= MEM_BUFFER_SIZE) {
		fh = sceIoOpen(TEMP_DOWNLOAD_NAME, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		sceIoWrite(fh, generic_mem_buffer, downloaded_bytes);
	}
ABORT_DOWNLOAD:
	sceIoClose(fh);
	downloaded_bytes = total_bytes;
	curl_easy_cleanup(curl_handle);
	return sceKernelExitDeleteThread(0);
}

bool download_file(char *url, char *text, bool cancelable, int custom_index, int custom_num_passes, GLuint bg_tex) {
	is_canceled = false;
	is_cancelable = cancelable;
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = 0;
	SceUID thd = sceKernelCreateThread("Generic Downloader", &downloadThread, 0x10000100, 0x100000, 0, 0, NULL);
	sprintf((char *)generic_url, url);
	sceKernelStartThread(thd, 0, NULL);
	do {
		int pass_idx = custom_index >= 0 ? custom_index : downloader_pass;
		int num_passes = custom_num_passes >= 0 ? custom_num_passes : 1;
		draw_downloader_dialog(pass_idx, downloaded_bytes, total_bytes, text, num_passes, true, bg_tex);
		res = sceKernelGetThreadInfo(thd, &info);
	} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
	
	if (is_cancelable) {
		return !is_canceled;
	}
	
	return true;
}

void silent_download(char *url) {
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = 0;
	SceUID thd = sceKernelCreateThread("Generic Downloader", &downloadMemThread, 0x10000100, 0x100000, 0, 0, NULL);
	sprintf((char *)generic_url, url);
	sceKernelStartThread(thd, 0, NULL);
	do {
		res = sceKernelGetThreadInfo(thd, &info);
	} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
}

void early_download_file(char *url, char *text) {
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = 0;
	SceUID thd = sceKernelCreateThread("Generic Downloader", &downloadThread, 0x10000100, 0x100000, 0, 0, NULL);
	sprintf((char *)generic_url, url);
	sceKernelStartThread(thd, 0, NULL);
	init_progressbar_dialog(text);
	do {
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
		sceMsgDialogProgressBarSetValue(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, (((float)downloaded_bytes / (float)total_bytes) * 100.0f));
		vglSwapBuffers(GL_TRUE);
		res = sceKernelGetThreadInfo(thd, &info);
	} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
	sceMsgDialogClose();
	int status = sceMsgDialogGetStatus();
	do {
		vglSwapBuffers(GL_TRUE);
		status = sceMsgDialogGetStatus();
	} while (status != SCE_COMMON_DIALOG_STATUS_FINISHED);
	sceMsgDialogTerm();
}

void stream_video(char *url) {
	is_canceled = false;
	is_cancelable = true;
	SceUID thd = sceKernelCreateThread("Video Streamer", &streamMemThread, 0x10000100, 0x100000, 0, 0, NULL);
	sprintf((char *)generic_url, url);
	sceKernelStartThread(thd, 0, NULL);
}

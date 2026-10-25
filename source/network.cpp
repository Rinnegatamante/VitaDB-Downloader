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
#include <string.h>
#include <sys/socket.h>
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
static volatile bool package_download_done = false;
static volatile bool package_download_failed = false;
volatile bool is_cancelable = false;
volatile bool is_canceled = false;
uint8_t *generic_mem_buffer = nullptr;
static SceUID fh;
char *bytes_string;
extern int SCE_CTRL_CANCEL;
extern SceUID video_stream_thid;

volatile uint64_t video_buffer_bytes = 0;
volatile int video_decoder_idx = 0;
volatile int video_downloader_idx = 0;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
	if (is_canceled) {
		return 0;
	}
	
	if (is_cancelable) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_CANCEL) {
			is_canceled = true;
			return 0;
		}
	}
	if (total_bytes > MEM_BUFFER_SIZE || fh >= 0) {
		if (fh < 0) {
			fh = sceIoOpen(TEMP_DOWNLOAD_NAME, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		}
		if (fh < 0 || sceIoWrite(fh, ptr, nmemb) != (int)nmemb) {
			package_download_failed = true;
			return 0;
		}
	} else {
		if (downloaded_bytes > MEM_BUFFER_SIZE || nmemb > MEM_BUFFER_SIZE - downloaded_bytes) {
			package_download_failed = true;
			return 0;
		}
		uint8_t *dst = &generic_mem_buffer[downloaded_bytes];
		sceClibMemcpy(dst, ptr, nmemb);
	}
	downloaded_bytes += nmemb;
	if (total_bytes < downloaded_bytes) {
		total_bytes = downloaded_bytes;
	}
	return nmemb;
}

static size_t write_video_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
	if (is_canceled) {
		return 0;
	}
	
	if (is_cancelable) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if ((pad.buttons & SCE_CTRL_CANCEL) || (pad.buttons & SCE_CTRL_START)) {
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
			if (is_canceled) {
				return 0;
			} else if (is_cancelable) {
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
		if (nmemb > (VIDEO_DECODER_BUFFER_SIZE - video_buffer_bytes)) {
			video_buffer_bytes = nmemb - (VIDEO_DECODER_BUFFER_SIZE - video_buffer_bytes);
		}
	} else {
		sceClibMemcpy(&generic_mem_buffer[video_downloader_idx * VIDEO_DECODER_BUFFER_SIZE + video_buffer_bytes], ptr, nmemb);
		video_buffer_bytes += nmemb;
	}
	downloaded_bytes += nmemb;
	return nmemb;
}

time_t download_tstamp = 0;
int response_code = 0;
static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
	size_t length = size * nitems;
	char line[512];
	if (length >= sizeof(line))
		return length;
	memcpy(line, buffer, length);
	line[length] = '\0';
	if (!strncasecmp(line, "Content-Length:", 15)) {
		unsigned long long response_length = 0;
		if (sscanf(line + 15, "%llu", &response_length) == 1) {
			total_bytes = response_length;
		}
	#ifdef DEBUG_NET
		sceClibPrintf("network.cpp: Detected total length of %llu bytes.\n", total_bytes);
	#endif
	} else if (!strncasecmp(line, "Json-Length:", 12)) {
		unsigned long long response_length = 0;
		if (sscanf(line + 12, "%llu", &response_length) == 1) {
			total_bytes = response_length;
		}
	#ifdef DEBUG_NET
		sceClibPrintf("network.cpp: Detected total length of %llu bytes.\n", total_bytes);
	#endif
	} else if (!strncasecmp(line, "Last-Modified:", 14)) {
		char date_string[128];
		const char *data_start = line + 14;
		while (*data_start == ' ' || *data_start == '\t')
			data_start++;
		size_t date_len = strlen(data_start);
		if (date_len >= sizeof(date_string)) {
			date_len = sizeof(date_string) - 1;
		}
		memcpy(date_string, data_start, date_len);
		date_string[date_len] = '\0';
		for (size_t i = 0; i < date_len; i++) {
			if (date_string[i] == '\r' || date_string[i] == '\n') {
				date_string[i] = '\0';
				break;
			}
		}
		time_t parsed_time = curl_getdate(date_string, NULL);
		if (parsed_time != -1) {
			download_tstamp = parsed_time;
#ifdef DEBUG_NET
			sceClibPrintf("network.cpp: Parsed Last-Modified timestamp: %d\n", download_tstamp);
#endif		
		}
	}
	return length;
}

static size_t header_dummy_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
	return size * nitems;
}

// A single larger socket receive window avoids long TCP stalls on the Vita.
static int serial_socket_options(void *dummy, curl_socket_t fd, curlsocktype dummy2) {
	const int receive_buffer = 524288;
	setsockopt((int)fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
	return CURL_SOCKOPT_OK;
}

struct transfer_state {
	uint64_t start_us;
	uint64_t last_progress_us;
	uint64_t last_poll_us;
	curl_off_t last_bytes;
};

struct resume_hdr {
	uint64_t first;
	uint64_t total;
	long status;
	bool content_range_valid;
	bool accepted;
};

static size_t resumed_header_cb(char *data, size_t size, size_t count, void *userdata) {
	resume_hdr *headers = (resume_hdr *)userdata;
	size_t length = size * count;
	char line[512];
	if (length >= sizeof(line))
		return length;
	memcpy(line, data, length);
	line[length] = '\0';
	if (!strncasecmp(line, "HTTP/", 5)) {
		const char *status = strchr(line, ' ');
		headers->status = status ? strtol(status + 1, NULL, 10) : 0;
		headers->content_range_valid = false;
		headers->accepted = false;
	} else if (!strncasecmp(line, "Content-Range:", 14)) {
		unsigned long long first = 0, last = 0, total = 0;
		if (sscanf(line + 14, " bytes %llu-%llu/%llu",
			&first, &last, &total) == 3 &&
			first == headers->first && last >= first &&
			total == headers->total && last < total)
			headers->content_range_valid = true;
	} else if ((line[0] == '\r' || line[0] == '\n') &&
		headers->status >= 200 && headers->status < 300) {
		headers->accepted = headers->status == 206 && headers->content_range_valid;
		if (!headers->accepted) {
			return 0; // Never append a full HTTP 200 response to a partial download.
		}
	}
	return length;
}

static int serial_progress_cb(void *userdata, curl_off_t, curl_off_t downloaded,
	curl_off_t, curl_off_t) {
	transfer_state *state = (transfer_state*)userdata;
	uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();
	if (downloaded > state->last_bytes)
		state->last_progress_us = now;
	state->last_bytes = downloaded;
	// Force a fresh connection after four seconds without received bytes.
	if ((downloaded > 0 && now - state->last_progress_us >= 4000000ULL) ||
		(downloaded == 0 && now - state->start_us >= 12000000ULL))
		return 1;
	if (is_cancelable && now - state->last_poll_us >= 100000ULL) {
		SceCtrlData pad;
		if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0 && (pad.buttons & SCE_CTRL_CANCEL))
			is_canceled = true;
		state->last_poll_us = now;
	}
	return is_cancelable && is_canceled ? 1 : 0;
}

static CURLcode startDownload(const char *url, time_t timestamp = 0,
	bool reliable = false) {
	response_code = 0;
	download_tstamp = 0;
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
	if (timestamp > 0) {
		curl_easy_setopt(curl_handle, CURLOPT_TIMECONDITION, (long)CURL_TIMECOND_IFMODSINCE);
		curl_easy_setopt(curl_handle, CURLOPT_TIMEVALUE, (long)timestamp);
	}
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, bytes_string); // Dummy
	curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, downloaded_bytes ? header_dummy_cb : header_cb);
	curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, bytes_string); // Dummy
	curl_easy_setopt(curl_handle, CURLOPT_RESUME_FROM, downloaded_bytes);
	curl_easy_setopt(curl_handle, CURLOPT_BUFFERSIZE, 524288);
	if (strstr(url, "/vitadb/get_psarc_url.php") || strstr(url, "/vitadb/get_psarc_data.php")) {
		curl_easy_setopt(curl_handle, CURLOPT_SOCKOPTFUNCTION, serial_socket_options);
	}
	transfer_state transfer = {};
	resume_hdr resume_headers = {};
	if (reliable) {
		transfer.start_us = (uint64_t)sceKernelGetSystemTimeWide();
		transfer.last_progress_us = transfer.start_us;
		curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, serial_progress_cb);
		curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, &transfer);
		curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_LIMIT, 1024L);
		curl_easy_setopt(curl_handle, CURLOPT_LOW_SPEED_TIME, 8L);
		curl_easy_setopt(curl_handle, CURLOPT_FRESH_CONNECT, 1L);
		curl_easy_setopt(curl_handle, CURLOPT_FORBID_REUSE, 1L);
		curl_easy_setopt(curl_handle, CURLOPT_FAILONERROR, 1L);
		if (downloaded_bytes > 0) {
			resume_headers.first = downloaded_bytes;
			resume_headers.total = total_bytes;
			curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, resumed_header_cb);
			curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, &resume_headers);
		}
	}
	struct curl_slist *headerchunk = NULL;
	headerchunk = curl_slist_append(headerchunk, "Accept: */*");
	headerchunk = curl_slist_append(headerchunk, "Content-Type: application/json");
	headerchunk = curl_slist_append(headerchunk, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/58.0.3029.110 Safari/537.36");
	headerchunk = curl_slist_append(headerchunk, "Content-Length: 0");
	curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headerchunk);
	uint64_t bytes_before = downloaded_bytes;
	CURLcode res = curl_easy_perform(curl_handle);
	if (res == CURLE_OK) {
		curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
	}
	if (reliable && res == CURLE_OK) {
		long http_status = 0;
		curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_status);
		if (http_status < 200 || http_status >= 300) {
			res = CURLE_HTTP_RETURNED_ERROR;
		}
		if (bytes_before > 0 && !resume_headers.accepted) {
			res = CURLE_HTTP_RETURNED_ERROR;
		}
	}
	curl_slist_free_all(headerchunk);
	return res;
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
	is_cancelable = false;
	is_canceled = false;
	curl_handle = curl_easy_init();
	downloader_pass = 1;
	downloaded_bytes = 0;
	fh = -1;
	time_t timestamp = 0;

	SceIoStat stat;
	if (sceIoGetstat("ux0:data/VitaDB/apps.json", &stat) >= 0) {
		total_bytes = stat.st_size;
		SceUID fh2 = sceIoOpen("ux0:data/VitaDB/apps.stamp", SCE_O_RDONLY, 0777);
		if (fh2 > 0) {
			sceIoRead(fh2, &timestamp, sizeof(time_t));
			sceIoClose(fh2);
		}
	} else {
		total_bytes = 12 * 1024;
	}

	while (downloaded_bytes < total_bytes && response_code != 304) {
		startDownload("https://www.rinnegatamante.eu/vitadb/list_hbs_json.php", timestamp);
	}

	if (downloaded_bytes > 12 * 1024 && response_code != 304) {
		fh = sceIoOpen("ux0:data/VitaDB/apps.json", SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		sceIoWrite(fh, generic_mem_buffer, downloaded_bytes);
		sceIoClose(fh);
		fh = sceIoOpen("ux0:data/VitaDB/apps.stamp", SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		sceIoWrite(fh, &download_tstamp, sizeof(time_t));
		sceIoClose(fh);
	}
	downloaded_bytes = total_bytes;
	curl_easy_cleanup(curl_handle);
	return sceKernelExitDeleteThread(0);
}

int appPspListThread(unsigned int args, void *arg) {
	is_cancelable = false;
	is_canceled = false;
	curl_handle = curl_easy_init();
	downloader_pass = 1;
	downloaded_bytes = 0;
	fh = -1;
	time_t timestamp = 0;

	SceIoStat stat;
	if (sceIoGetstat("ux0:data/VitaDB/psp_apps.json", &stat) >= 0) {
		total_bytes = stat.st_size;
		SceUID fh2 = sceIoOpen("ux0:data/VitaDB/psp_apps.stamp", SCE_O_RDONLY, 0777);
		if (fh2 > 0) {
			sceIoRead(fh2, &timestamp, sizeof(time_t));
			sceIoClose(fh2);
		}
	} else {
		total_bytes = 12 * 1024;
	}

	while (downloaded_bytes < total_bytes && response_code != 304) {
		startDownload("https://www.rinnegatamante.eu/vitadb/list_psp_hbs_json.php", timestamp);
	}

	if (downloaded_bytes > 12 * 1024 && response_code != 304) {
		fh = sceIoOpen("ux0:data/VitaDB/psp_apps.json", SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		sceIoWrite(fh, generic_mem_buffer, downloaded_bytes);
		sceIoClose(fh);
		fh = sceIoOpen("ux0:data/VitaDB/psp_apps.stamp", SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		sceIoWrite(fh, &download_tstamp, sizeof(time_t));
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
	curl_easy_cleanup(curl_handle);
	return sceKernelExitDeleteThread(0);
}

int downloadThread(unsigned int args, void *arg) {
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
	// Only PSARC packages use the more aggressive timeout and safe resume path.
	const bool reliable = strstr(final_url, "/vitadb/get_psarc_url.php") || strstr(final_url, "/vitadb/get_psarc_data.php");
	const uint64_t started = (uint64_t)sceKernelGetSystemTimeWide();
	int attempts = 0;
	int empty_attempts = 0;
	curl_handle = curl_easy_init();
	if (!curl_handle) {
		package_download_failed = true;
		goto ABORT_DOWNLOAD;
	}
	do {
		uint64_t before = downloaded_bytes;
		CURLcode result = startDownload(final_url, 0, reliable);
		if (is_cancelable && is_canceled) {
			goto ABORT_DOWNLOAD;
		}
		if (package_download_failed) {
			break;
		}
		if (reliable && (result == CURLE_HTTP_RETURNED_ERROR || (result != CURLE_OK && downloaded_bytes >= total_bytes))) {
			package_download_failed = true;
			break;
		}
		if (reliable && downloaded_bytes < total_bytes) {
			empty_attempts = downloaded_bytes == before ? empty_attempts + 1 : 0;
			if (++attempts >= 12 || empty_attempts >= 3 || (uint64_t)sceKernelGetSystemTimeWide() - started >= 120000000ULL) {
				package_download_failed = true;
				break;
			}
			sceKernelDelayThread(250000);
		}
	} while (downloaded_bytes < total_bytes);
	if (!package_download_failed && !is_canceled && downloaded_bytes > 180 && total_bytes <= MEM_BUFFER_SIZE) {
		fh = sceIoOpen(TEMP_DOWNLOAD_NAME, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
		if (fh < 0 || sceIoWrite(fh, generic_mem_buffer, downloaded_bytes) !=
			(int)downloaded_bytes)
			package_download_failed = true;
	} else if (reliable && downloaded_bytes <= 180) {
		package_download_failed = true;
	}
ABORT_DOWNLOAD:
	if (fh >= 0 && sceIoClose(fh) < 0) {
		package_download_failed = true;
	}
	if (package_download_failed || is_canceled) {
		sceIoRemove(TEMP_DOWNLOAD_NAME);
	}
	downloaded_bytes = total_bytes;
	if (curl_handle) {
		curl_easy_cleanup(curl_handle);
	}
	package_download_done = true;
	return sceKernelExitDeleteThread(0);
}

bool download_file(char *url, char *text, bool cancelable, int custom_index, int custom_num_passes, GLuint bg_tex) {
	is_canceled = false;
	is_cancelable = cancelable;
	package_download_done = false;
	package_download_failed = false;
	SceUID thd = sceKernelCreateThread("Generic Downloader", &downloadThread, 0x10000100, 0x100000, 0, 0, NULL);
	if (thd < 0) {
		return false;
	}
	sprintf((char *)generic_url, url);
	sceKernelStartThread(thd, 0, NULL);
	do {
		int pass_idx = custom_index >= 0 ? custom_index : downloader_pass;
		int num_passes = custom_num_passes >= 0 ? custom_num_passes : 1;
		draw_downloader_dialog(pass_idx, downloaded_bytes, total_bytes, text, num_passes, true, bg_tex);
	} while (!package_download_done);
	
	if (is_cancelable) {
		return !is_canceled && !package_download_failed;
	}

	return !package_download_failed;
}

void silent_download(char *url) {
	is_canceled = false;
	is_cancelable = false;
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
	is_canceled = false;
	is_cancelable = false;
	package_download_failed = false;
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
	video_stream_thid = sceKernelCreateThread("Video Streamer", &streamMemThread, 0x10000100, 0x100000, 0, 0, NULL);
	sprintf((char *)generic_url, url);
	sceKernelStartThread(video_stream_thid, 0, NULL);
}

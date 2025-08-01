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

#ifndef _NETWORK_H
#define _NETWORK_H

#define MEM_BUFFER_SIZE (32 * 1024 * 1024)
#define VIDEO_DECODER_BUFFER_SIZE (MEM_BUFFER_SIZE / 8)

#define TEMP_DOWNLOAD_NAME "ux0:data/VitaDB/temp.tmp"

extern uint8_t *generic_mem_buffer;
extern volatile uint64_t total_bytes;
extern volatile uint64_t downloaded_bytes;
extern volatile uint8_t downloader_pass;

extern int video_decoder_idx;
extern int video_downloader_idx;

int appListThread(unsigned int args, void *arg);
int appPspListThread(unsigned int args, void *arg);
int downloadThread(unsigned int args, void *arg);

bool download_file(char *url, char *text, bool cancelable = false, int custom_index = -1, int custom_num_passes = -1, GLuint bg_tex = 0);
void silent_download(char *url);
void early_download_file(char *url, char *text);
void stream_video(char *url);

#endif
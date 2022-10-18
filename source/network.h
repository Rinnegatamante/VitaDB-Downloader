/*
 * This file is part of VitaDB Downloader
 * Copyright 2022 Rinnegatamante
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
#define TEMP_DOWNLOAD_NAME "ux0:data/VitaDB/temp.tmp"

extern uint8_t *generic_mem_buffer;
extern volatile uint64_t total_bytes;
extern volatile uint64_t downloaded_bytes;
extern volatile uint8_t downloader_pass;

int appListThread(unsigned int args, void *arg);
int appPspListThread(unsigned int args, void *arg);
int downloadThread(unsigned int args, void *arg);

void download_file(char *url, char *text);
void early_download_file(char *url, char *text);

#endif
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
 
#include <iostream>
#include <string>
#include <locale>
#include <codecvt>
#include <stdio.h>
#include <string>
#include <vitasdk.h>

static const char *sizes[] = {
	"B",
	"KB",
	"MB",
	"GB"
};

float format_size(float len) {
	while (len > 1024) len = len / 1024.0f;
	return len;
}

const char *format_size_str(uint64_t len) {
	uint8_t ret = 0;
	while (len > 1024) {
		ret++;
		len = len / 1024;
	}
	return sizes[ret];
}

void recursive_rmdir(const char *path) {
	SceUID d = sceIoDopen(path);
	if (d >= 0) {
		SceIoDirent g_dir;
		while (sceIoDread(d, &g_dir) > 0) {
			char fpath[512];
			sprintf(fpath, "%s/%s", path, g_dir.d_name);
			if (SCE_S_ISDIR(g_dir.d_stat.st_mode))
				recursive_rmdir(fpath);
			else
				sceIoRemove(fpath);
		}
		sceIoDclose(d);
		sceIoRmdir(path);
	}
}

void recursive_mkdir(char *dir) {
	char *p = dir;
	while (p) {
		char *p2 = strstr(p, "/");
		if (p2) {
			p2[0] = 0;
			sceIoMkdir(dir, 0777);
			p = p2 + 1;
			p2[0] = '/';
		} else break;
	}
}

uint64_t get_free_storage() {
	uint64_t free_storage, dummy;
	SceIoDevInfo info;
	int res = sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(SceIoDevInfo));
	if (res >= 0)
		free_storage = info.free_size;
	else
		sceAppMgrGetDevInfo("ux0:", &dummy, &free_storage);
	
	return free_storage;
}

uint64_t get_total_storage() {
	uint64_t total_storage, dummy;
	SceIoDevInfo info;
	int res = sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(SceIoDevInfo));
	if (res >= 0)
		total_storage = info.max_size;
	else
		sceAppMgrGetDevInfo("ux0:", &total_storage, &dummy);
	
	return total_storage;
}

char *unescape(char *src) {
	char *res = malloc(strlen(src) + 1);
	uint32_t i = 0;
	char *s = src;
	while (*s) {
		char c = *s;
		int incr = 1;
		if (c == '\\' && s[1]) {
			switch (s[1]) {
			case '\\':
				c = '\\';
				incr = 2;
				break;
			case 'n':
				c = '\n';
				incr = 2;
				break;
			case 't':
				c = '\t';
				incr = 2;
				break;
			case '\'':
				c = '\'';
				incr = 2;
				break;
			case '\"':
				c = '\"';
				incr = 2;
				break;
			default:
				break;
			}
		}
		res[i++] = c;
		s += incr;
	}
	res[i] = 0;
	free(src);
	return res;
}

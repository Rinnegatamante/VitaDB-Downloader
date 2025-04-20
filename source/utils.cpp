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
#include <codecvt>
#include <stdio.h>
#include <string>
#include <vitasdk.h>
#include <vitaGL.h>
#include <imgui_vita.h>
#include <imgui_internal.h>
#include "adrenaline.h"
#include "network.h"
#include "utils.h"
#include "md5.h"

#define SCE_ERROR_ERRNO_EEXIST 0x80010011

char pspemu_dev[8];

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

void copy_file(const char *src, const char *dst) {
	SceUID fs = sceIoOpen(src, SCE_O_RDONLY, 0777);
	SceUID fd = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
	size_t fsize;
	while (fsize = sceIoRead(fs, generic_mem_buffer, MEM_BUFFER_SIZE)) {
		sceIoWrite(fd, generic_mem_buffer, fsize);
	}
	sceIoClose(fs);
	sceIoClose(fd);
}

void move_path(char *src, char *dst) {
	if (src[strlen(src) - 1] == '/')
		src[strlen(src) - 1] = 0;
	if (dst[strlen(dst) - 1] == '/')
		dst[strlen(dst) - 1] = 0;
	if (sceIoRename(src, dst) == SCE_ERROR_ERRNO_EEXIST) {
		SceIoStat src_stat, dst_stat;
		sceIoGetstat(src, &src_stat);
		sceIoGetstat(dst, &dst_stat);
		bool src_is_dir = SCE_S_ISDIR(src_stat.st_mode);
		bool dst_is_dir = SCE_S_ISDIR(dst_stat.st_mode);
		if (src_is_dir != dst_is_dir) {
			if (dst_is_dir)
				recursive_rmdir(dst);
			else
				sceIoRemove(dst);
			sceIoRename(src, dst);
		} else {
			if (src_is_dir) {
				SceUID dfd = sceIoDopen(src);
				int r = 0;
				SceIoDirent dir;
				while (r = sceIoDread(dfd, &dir) > 0) {
					char spath[512], dpath[512];
					sprintf(spath, "%s/%s", src, dir.d_name);
					sprintf(dpath, "%s/%s", dst, dir.d_name);
					move_path(spath, dpath);
				}
				sceIoDclose(dfd);
				sceIoRmdir(src);
			} else {
				sceIoRemove(dst);
				sceIoRename(src, dst);
			}
		}
	}
}

void recursive_rmdir(char *path) {
	if (path[strlen(path) - 1] == '/')
		path[strlen(path) - 1] = 0;
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

void populate_pspemu_path() {
	SceUID f = sceIoOpen("ux0:app/PSPEMUCFW/adrenaline.bin", SCE_O_RDONLY, 0777);
	if (f >= 0) {
		AdrenalineConfig cfg;
		sceIoRead(f, &cfg, sizeof(AdrenalineConfig));
		sceIoClose(f);
		if (cfg.magic[0] == ADRENALINE_CFG_MAGIC_1 && cfg.magic[1] == ADRENALINE_CFG_MAGIC_2) {
			switch (cfg.ms_location) {
			case MEMORY_STICK_LOCATION_UR0:
				strcpy(pspemu_dev, "ur0:");
				break;
			case MEMORY_STICK_LOCATION_IMC0:
				strcpy(pspemu_dev, "imc0:");
				break;
			case MEMORY_STICK_LOCATION_XMC0:
				strcpy(pspemu_dev, "xmc0:");
				break;
			case MEMORY_STICK_LOCATION_UMA0:	
				strcpy(pspemu_dev, "uma0:");
				break;
			default:
				strcpy(pspemu_dev, "ux0:");
				break;
			}
			return;
		}
	} 
	strcpy(pspemu_dev, "ux0:");
}

uint64_t get_free_storage() {
	uint64_t free_storage, dummy;
	SceIoDevInfo info;
	int res = sceIoDevctl(mode_idx == MODE_PSP_HBS ? pspemu_dev : "ux0:", 0x3001, NULL, 0, &info, sizeof(SceIoDevInfo));
	if (res >= 0)
		free_storage = info.free_size;
	else
		sceAppMgrGetDevInfo(mode_idx == MODE_PSP_HBS ? pspemu_dev : "ux0:", &dummy, &free_storage);
	
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
	char *res = (char *)malloc(strlen(src) + 1);
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

void calculate_md5(SceUID f, char *hash) {
	MD5Context ctx;
	MD5Init(&ctx);
	size_t file_size = sceIoLseek(f, 0, SCE_SEEK_END);
	sceIoLseek(f, 0, SCE_SEEK_SET);
	size_t read_size = 0;
	while (read_size < file_size) {
		size_t read_buf_size = (file_size - read_size) > MEM_BUFFER_SIZE ? MEM_BUFFER_SIZE : (file_size - read_size);
		sceIoRead(f, generic_mem_buffer, read_buf_size);
		read_size += read_buf_size;
		MD5Update(&ctx, generic_mem_buffer, read_buf_size);
	}
	sceIoClose(f);
	unsigned char md5_buf[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	MD5Final(md5_buf, &ctx);
	sprintf(hash, "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
		md5_buf[0], md5_buf[1], md5_buf[2], md5_buf[3], md5_buf[4], md5_buf[5], md5_buf[6], md5_buf[7], md5_buf[8],
		md5_buf[9], md5_buf[10], md5_buf[11], md5_buf[12], md5_buf[13], md5_buf[14], md5_buf[15]);
}

#define CIRCLE_SEGMENTS_NUM 30
namespace ImGui {
void ImageRound(ImTextureID user_texture_id, const ImVec2 &size) {
	ImGuiWindow *window = ImGui::GetCurrentWindow();
	ImRect bb(window->DC.CursorPos, ImVec2(window->DC.CursorPos.x + size.x, window->DC.CursorPos.y + size.y));
	ImGui::ItemSize(bb);
	if (!ImGui::ItemAdd(bb, 0))
		return;
	
	window->DrawList->PushTextureID(user_texture_id);

	int vert_start_idx = window->DrawList->VtxBuffer.Size;
	window->DrawList->AddCircleFilled(ImVec2(bb.Min.x + size.x / 2.f, bb.Min.y + size.y / 2.f), size.x / 2.f, 0xFFFFFFFF, CIRCLE_SEGMENTS_NUM);
	int vert_end_idx = window->DrawList->VtxBuffer.Size;
	ImGui::ShadeVertsLinearUV(window->DrawList->VtxBuffer.Data + vert_start_idx, window->DrawList->VtxBuffer.Data + vert_end_idx, bb.Min, bb.Max, ImVec2(0, 0), ImVec2(1, 1), true);

	window->DrawList->PopTextureID();
}
}

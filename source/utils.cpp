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
#include <vitaGL.h>
#include <imgui_vita.h>
#include <imgui_internal.h>
#include "adrenaline.h"
#include "network.h"
#include "utils.h"

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
	FILE *fs = fopen(src, "r");
	FILE *fd = fopen(dst, "w");
	size_t fsize = fread(generic_mem_buffer, 1, MEM_BUFFER_SIZE, fs);
	fwrite(generic_mem_buffer, 1, fsize, fd);
	fclose(fs);
	fclose(fd);
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

void populate_pspemu_path() {
	FILE *f = fopen("ux0:app/PSPEMUCFW/adrenaline.bin", "r");
	if (f) {
		AdrenalineConfig cfg;
		fread(&cfg, 1, sizeof(AdrenalineConfig), f);
		fclose(f);
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

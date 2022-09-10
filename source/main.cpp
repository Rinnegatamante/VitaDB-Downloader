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
#include <vitasdk.h>
#include <vitaGL.h>
#include <imgui_vita.h>
#include <imgui_internal.h>
#include <bzlib.h>
#include <stdio.h>
#include <string>
#include <soloud.h>
#include <soloud_wav.h>
#include "unzip.h"
#include "player.h"
#include "promoter.h"
#include "utils.h"
#include "dialogs.h"
#include "network.h"
#include "md5.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define VERSION "1.4"

#define MIN(x, y) (x) < (y) ? (x) : (y)
#define PREVIEW_PADDING 6
#define PREVIEW_HEIGHT 128.0f
#define PREVIEW_WIDTH  128.0f

int _newlib_heap_size_user = 200 * 1024 * 1024;
int filter_idx = 0;
int cur_ss_idx;
int old_ss_idx = -1;

static char download_link[512];
char app_name_filter[128] = {0};

enum {
	APP_UNTRACKED,
	APP_OUTDATED,
	APP_UPDATED
};

struct AppSelection {
	char name[192];
	char icon[128];
	char author[128];
	char type[2];
	char id[8];
	char date[12];
	char titleid[10];
	char screenshots[512];
	char *desc;
	char downloads[16];
	char size[16];
	char data_size[16];
	char hash[34];
	char *requirements;
	char data_link[128];
	int state;
	AppSelection *next;
};

static AppSelection *old_hovered = NULL;
AppSelection *apps = nullptr;

char *extractValue(char *dst, char *src, char *val, char **new_ptr) {
	char label[32];
	sprintf(label, "\"%s\": \"", val);
	//printf("label: %s\n", label);
	char *ptr = strstr(src, label) + strlen(label);
	//printf("ptr is: %X\n", ptr);
	if (ptr == strlen(label))
		return nullptr;
	char *end2 = strstr(ptr, val[0] == 'l' ? "\"," : "\"");
	if (dst == nullptr) {
		if (end2 - ptr > 0) {
			dst = malloc(end2 - ptr + 1);
			*new_ptr = dst;
		} else {
			*new_ptr = nullptr;
			return end2 + 1;
		}
	}
	//printf("size: %d\n", end2 - ptr);
	memcpy(dst, ptr, end2 - ptr);
	dst[end2 - ptr] = 0;
	return end2 + 1;
}

char *GetChangelog(const char *file, char *id) {
	char *res = nullptr;
	FILE *f = fopen(file, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		uint64_t len = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		fread(buffer, 1, len, f);
		buffer[len] = 0;
		char *ptr = buffer;
		char *end, *end2;
		char cur_id[8];
		do {
			ptr = extractValue(cur_id, ptr, "id", nullptr);
			if (!strncmp(cur_id, id, 3)) {
				ptr = extractValue(res, ptr, "changelog", &res);
				res = unescape(res);
				break;
			}
		} while (ptr);
		fclose(f);
		free(buffer);
	}
	return res;
}

bool update_detected = false;
void AppendAppDatabase(const char *file) {
	// Burning on screen the parsing text dialog
	for (int i = 0; i < 3; i++) {
		DrawTextDialog("Parsing apps list", true, true);
	}
	FILE *f = fopen(file, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		uint64_t len = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		fread(buffer, 1, len, f);
		buffer[len] = 0;
		char *ptr = buffer;
		char *end, *end2;
		do {
			char name[128], version[64], fname[64], cur_hash[40];
			ptr = extractValue(name, ptr, "name", nullptr);
			//printf("parsing %s\n", name);
			if (!ptr)
				break;
			AppSelection *node = (AppSelection*)malloc(sizeof(AppSelection));
			node->desc = nullptr;
			node->requirements = nullptr;
			ptr = extractValue(node->icon, ptr, "icon", nullptr);
			ptr = extractValue(version, ptr, "version", nullptr);
			ptr = extractValue(node->author, ptr, "author", nullptr);
			ptr = extractValue(node->type, ptr, "type", nullptr);
			ptr = extractValue(node->id, ptr, "id", nullptr);
			if (!strncmp(node->id, "877", 3)) { // VitaDB Downloader, check if newer than running version
				if (strncmp(&version[2], VERSION, 3))
					update_detected = true;
			}
			ptr = extractValue(node->date, ptr, "date", nullptr);
			ptr = extractValue(node->titleid, ptr, "titleid", nullptr);
			ptr = extractValue(node->screenshots, ptr, "screenshots", nullptr);
			ptr = extractValue(node->desc, ptr, "long_description", &node->desc);
			node->desc = unescape(node->desc);
			ptr = extractValue(node->downloads, ptr, "downloads", nullptr);
			ptr = extractValue(node->size, ptr, "size", nullptr);
			ptr = extractValue(node->data_size, ptr, "data_size", nullptr);
			ptr = extractValue(node->hash, ptr, "hash", nullptr);
			//printf("db hash %s\n", node->hash);
			sprintf(fname, "ux0:app/%s/hash.vdb", node->titleid);
			FILE *f  = fopen(fname, "r");
			if (f) {
				//printf("found hash file\n");
				fread(cur_hash, 1, 32, f);
				cur_hash[32] = 0;
				fclose(f);
				//printf("local hash %s\n", cur_hash);
				if (strncmp(cur_hash, node->hash, 32))
					node->state = APP_OUTDATED;
				else
					node->state = APP_UPDATED;
			} else {
				//printf("hash file not found, calculating md5\n");
				sprintf(fname, "ux0:app/%s/eboot.bin", node->titleid);
				f = fopen(fname, "r");
				if (f) {
					//printf("eboot.bin found, starting md5sum\n");
					MD5Context ctx;
					MD5Init(&ctx);
					fseek(f, 0, SEEK_END);
					int file_size = ftell(f);
					fseek(f, 0, SEEK_SET);
					int read_size = 0;
					while (read_size < file_size) {
						int read_buf_size = (file_size - read_size) > MEM_BUFFER_SIZE ? MEM_BUFFER_SIZE : (file_size - read_size);
						fread(generic_mem_buffer, 1, read_buf_size, f);
						read_size += read_buf_size;
						MD5Update(&ctx, generic_mem_buffer, read_buf_size);
					}
					fclose(f);
					unsigned char md5_buf[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
					MD5Final(md5_buf, &ctx);
					sprintf(cur_hash, "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
						md5_buf[0], md5_buf[1], md5_buf[2], md5_buf[3], md5_buf[4], md5_buf[5], md5_buf[6], md5_buf[7], md5_buf[8],
						md5_buf[9], md5_buf[10], md5_buf[11], md5_buf[12], md5_buf[13], md5_buf[14], md5_buf[15]);
					//printf("local hash %s\n", cur_hash);
					if (strncmp(cur_hash, node->hash, 32))
						node->state = APP_OUTDATED;
					else
						node->state = APP_UPDATED;
					sprintf(fname, "ux0:app/%s/hash.vdb", node->titleid);
					f = fopen(fname, "w");
					fwrite(cur_hash, 1, 32, f);
					fclose(f);
				} else
					node->state = APP_UNTRACKED;
			}
			//printf("hash part done\n");
			ptr = extractValue(node->requirements, ptr, "requirements", &node->requirements);
			if (node->requirements)
				node->requirements = unescape(node->requirements);
			ptr = extractValue(node->data_link, ptr, "data", nullptr);
			sprintf(node->name, "%s %s", name, version);
			node->next = apps;
			apps = node;
		} while (ptr);
		fclose(f);
		free(buffer);
	}
	//printf("finished parsing\n");
}

static char fname[512], ext_fname[512], read_buffer[8192];

void extract_file(char *file, char *dir) {
	unz_global_info global_info;
	unz_file_info file_info;
	unzFile zipfile = unzOpen(file);
	unzGetGlobalInfo(zipfile, &global_info);
	unzGoToFirstFile(zipfile);
	uint64_t total_extracted_bytes = 0;
	uint64_t curr_extracted_bytes = 0;
	uint64_t curr_file_bytes = 0;
	int num_files = global_info.number_entry;
	for (int zip_idx = 0; zip_idx < num_files; ++zip_idx) {
		unzGetCurrentFileInfo(zipfile, &file_info, fname, 512, NULL, 0, NULL, 0);
		total_extracted_bytes += file_info.uncompressed_size;
		if ((zip_idx + 1) < num_files) unzGoToNextFile(zipfile);
	}
	unzGoToFirstFile(zipfile);
	for (int zip_idx = 0; zip_idx < num_files; ++zip_idx) {
		unzGetCurrentFileInfo(zipfile, &file_info, fname, 512, NULL, 0, NULL, 0);
		sprintf(ext_fname, "%s%s", dir, fname); 
		const size_t filename_length = strlen(ext_fname);
		if (ext_fname[filename_length - 1] != '/') {
			curr_file_bytes = 0;
			unzOpenCurrentFile(zipfile);
			recursive_mkdir(ext_fname);
			FILE *f = fopen(ext_fname, "wb");
			while (curr_file_bytes < file_info.uncompressed_size) {
				int rbytes = unzReadCurrentFile(zipfile, read_buffer, 8192);
				if (rbytes > 0) {
					fwrite(read_buffer, 1, rbytes, f);
					curr_extracted_bytes += rbytes;
					curr_file_bytes += rbytes;
				}
				DrawExtractorDialog(zip_idx + 1, curr_file_bytes, curr_extracted_bytes, file_info.uncompressed_size, total_extracted_bytes, fname, num_files);
			}
			fclose(f);
			unzCloseCurrentFile(zipfile);
		}
		if ((zip_idx + 1) < num_files) unzGoToNextFile(zipfile);
	}
	unzClose(zipfile);
	ImGui::GetIO().MouseDrawCursor = false;
}

static int preview_width, preview_height, preview_x, preview_y;
GLuint preview_icon = 0, preview_shot = 0, previous_frame = 0, bg_image = 0;
bool need_icon = false;
int show_screenshots = 0; // 0 = off, 1 = download, 2 = show
void LoadPreview(AppSelection *game) {
	if (old_hovered == game)
		return;
	old_hovered = game;
	
	char banner_path[256];
	sprintf(banner_path, "ux0:data/VitaDB/icons/%s", game->icon);
	uint8_t *icon_data = stbi_load(banner_path, &preview_width, &preview_height, NULL, 4);
	if (icon_data) {
		if (!preview_icon)
			glGenTextures(1, &preview_icon);
		glBindTexture(GL_TEXTURE_2D, preview_icon);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, preview_width, preview_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon_data);
		float scale = MIN(PREVIEW_WIDTH / (float)preview_width, PREVIEW_HEIGHT / (float)preview_height);
		preview_width = scale * (float)preview_width;
		preview_height = scale * (float)preview_height;
		preview_x = (PREVIEW_WIDTH - preview_width) / 2;
		preview_y = (PREVIEW_HEIGHT - preview_height) / 2;
		free(icon_data);
	} else {
		need_icon = true;
	}
}

void LoadScreenshot() {
	if (old_ss_idx == cur_ss_idx)
		return;
	bool is_incrementing = true;
	if (cur_ss_idx < 0) { 
		cur_ss_idx = 3;
		is_incrementing = false;
	} else if (cur_ss_idx > 3) {
		cur_ss_idx = 0;
	} else if (old_ss_idx > cur_ss_idx) {
		is_incrementing = false;
	}
	old_ss_idx = cur_ss_idx;
	
	char banner_path[256];
	sprintf(banner_path, "ux0:data/VitaDB/ss%d.png", cur_ss_idx);
	int w, h;
	uint8_t *shot_data = stbi_load(banner_path, &w, &h, NULL, 4);
	while (!shot_data) {
		cur_ss_idx += is_incrementing ? 1 : -1;
		if (cur_ss_idx < 0) { 
			cur_ss_idx = 3;
		} else if (cur_ss_idx > 3) {
			cur_ss_idx = 0;
		}
		sprintf(banner_path, "ux0:data/VitaDB/ss%d.png", cur_ss_idx);
		shot_data = stbi_load(banner_path, &w, &h, NULL, 4);
	}
	if (!preview_shot)
		glGenTextures(1, &preview_shot);
	glBindTexture(GL_TEXTURE_2D, preview_shot);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, shot_data);
	free(shot_data);
}

bool has_animated_bg = false;
void LoadBackground() {
	int w, h;
	
	FILE *f = fopen("ux0:data/VitaDB/bg.mp4", "rb");
	if (f) {
		fclose(f);
		video_open("ux0:data/VitaDB/bg.mp4");
		has_animated_bg = true;
	} else {
		uint8_t *bg_data = stbi_load("ux0:data/VitaDB/bg.png", &w, &h, NULL, 4);
		if (bg_data) {
			glGenTextures(1, &bg_image);
			glBindTexture(GL_TEXTURE_2D, bg_image);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bg_data);
			free(bg_data);
		}
	}
}

void swap_apps(AppSelection *a, AppSelection *b) {
	AppSelection tmp;
	
	// Swapping everything except next leaf pointer
	sceClibMemcpy(&tmp, a, sizeof(AppSelection) - 4);
	sceClibMemcpy(a, b, sizeof(AppSelection) - 4);
	sceClibMemcpy(b, &tmp, sizeof(AppSelection) - 4);
}

const char *sort_modes_str[] = {
	"Most Recent",
	"Oldest",
	"Most Downloaded",
	"Least Downloaded",
	"Alphabetical (A-Z)",
	"Alphabetical (Z-A)",
	"Smallest",
	"Largest"
};
const char *filter_modes[] = {
	"All Apps",
	"Original Games",
	"Game Ports",
	"Utilities",
	"Emulators",
	"Not Installed Apps",
	"Outdated Apps",
	"Installed Apps",
};
int sort_idx = 0;
int old_sort_idx = -1;

void sort_applist(AppSelection *start) { 
	// Checking for empty list
	if (start == NULL) 
		return; 
	
	int swapped; 
	AppSelection *ptr1; 
	AppSelection *lptr = NULL; 
  
	do { 
		swapped = 0; 
		ptr1 = start; 
		
		int64_t d1, d2;
		char * dummy;
		while (ptr1->next != lptr && ptr1->next) {
			switch (sort_idx) {
			case 0: // Most Recent
				if (strcasecmp(ptr1->date, ptr1->next->date) < 0) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 1: // Oldest
				if (strcasecmp(ptr1->date, ptr1->next->date) > 0) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 2: // Most Downloaded
				d1 = strtoll(ptr1->downloads, &dummy, 10);
				d2 = strtoll(ptr1->next->downloads, &dummy, 10);
				if (d1 < d2) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 3: // Least Downloaded
				d1 = strtoll(ptr1->downloads, &dummy, 10);
				d2 = strtoll(ptr1->next->downloads, &dummy, 10);
				if (d1 > d2) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 4: // A-Z
				if (strcasecmp(ptr1->name, ptr1->next->name) > 0) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 5: // Z-A
				if (strcasecmp(ptr1->name, ptr1->next->name) < 0) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 6: // Smallest
				d1 = strtoll(ptr1->size, &dummy, 10) + strtoll(ptr1->data_size, &dummy, 10);
				d2 = strtoll(ptr1->next->size, &dummy, 10) + strtoll(ptr1->next->data_size, &dummy, 10);
				if (d1 > d2) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 7: // Largest
				d1 = strtoll(ptr1->size, &dummy, 10) + strtoll(ptr1->data_size, &dummy, 10);
				d2 = strtoll(ptr1->next->size, &dummy, 10) + strtoll(ptr1->next->data_size, &dummy, 10);
				if (d1 < d2) {
					swap_apps(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			default:
				break;
			}
			ptr1 = ptr1->next; 
		} 
		lptr = ptr1; 
	} while (swapped); 
}

bool filterApps(AppSelection *p) {
	if (filter_idx) {
		int filter_cat = filter_idx > 2 ? (filter_idx + 1) : filter_idx;
		if (filter_cat <= 5) {
			if (p->type[0] - '0' != filter_cat)
				return true;
		} else {
			filter_cat -= 6;
			if (filter_cat < 2) {
				if (p->state != filter_cat)
					return true;
			} else {
				if (p->state == APP_UNTRACKED)
					return true;
			}
		}
	}
	return false;
}

static int musicThread(unsigned int args, void *arg) {
	// Starting background music
	SoLoud::Soloud audio_engine;
	SoLoud::Wav bg_mus;
	audio_engine.init();
	if (bg_mus.load("ux0:/data/VitaDB/bg.ogg") >= 0) {
		bg_mus.setLooping(true);
		audio_engine.playBackground(bg_mus);
	} else {
		return sceKernelExitDeleteThread(0);
	}
	for (;;) {
		sceKernelDelayThread(500 * 1000 * 1000);
	}
}

float *bg_attributes = nullptr;
void DrawBackground() {
	if (!bg_attributes)
		bg_attributes = (float*)malloc(sizeof(float) * 22);

	if (has_animated_bg) {
		int anim_w, anim_h;
		GLuint anim_bg = video_get_frame(&anim_w, &anim_h);
		if (anim_bg == 0xDEADBEEF)
			return;
		glBindTexture(GL_TEXTURE_2D, anim_bg);
	} else {
		glBindTexture(GL_TEXTURE_2D, bg_image);
	}
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	
	bg_attributes[0] = 0.0f;
	bg_attributes[1] = 0.0f;
	bg_attributes[2] = 0.0f;
	bg_attributes[3] = 960.0f;
	bg_attributes[4] = 0.0f;
	bg_attributes[5] = 0.0f;
	bg_attributes[6] = 0.0f;
	bg_attributes[7] = 544.0f;
	bg_attributes[8] = 0.0f;
	bg_attributes[9] = 960.0f;
	bg_attributes[10] = 544.0f;
	bg_attributes[11] = 0.0f;
	vglVertexPointerMapped(bg_attributes);
	
	bg_attributes[12] = 0.0f;
	bg_attributes[13] = 0.0f;
	bg_attributes[14] = 1.0f;
	bg_attributes[15] = 0.0f;
	bg_attributes[16] = 0.0f;
	bg_attributes[17] = 1.0f;
	bg_attributes[18] = 1.0f;
	bg_attributes[19] = 1.0f;
	vglTexCoordPointerMapped(&bg_attributes[12]);
	
	uint16_t *bg_indices = (uint16_t*)&bg_attributes[20];
	bg_indices[0] = 0;
	bg_indices[1] = 1;
	bg_indices[2] = 2;
	bg_indices[3] = 3;
	vglIndexPointerMapped(bg_indices);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0, 960, 544, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	vglDrawObjects(GL_TRIANGLE_STRIP, 4, GL_TRUE);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDisableClientState(GL_COLOR_ARRAY);
}

ImVec4 TextLabel = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
ImVec4 TextOutdated = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
ImVec4 TextUpdated = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);

#define READ_FIRST_VAL(x) if (strcmp(#x, buffer) == 0) style.Colors[ImGuiCol_##x] = ImVec4(values[0], values[1], values[2], values[3]);
#define READ_NEXT_VAL(x) else if (strcmp(#x, buffer) == 0) style.Colors[ImGuiCol_##x] = ImVec4(values[0], values[1], values[2], values[3]);
#define READ_EXTRA_VAL(x) else if (strcmp(#x, buffer) == 0) x = ImVec4(values[0], values[1], values[2], values[3]);
#define WRITE_VAL(a) fprintf(f, "%s=%f,%f,%f,%f\n", #a, style.Colors[ImGuiCol_##a].x, style.Colors[ImGuiCol_##a].y, style.Colors[ImGuiCol_##a].z, style.Colors[ImGuiCol_##a].w);
#define WRITE_EXTRA_VAL(a) fprintf(f, "%s=%f,%f,%f,%f\n", #a, a.x, a.y, a.z, a.w);
void set_gui_theme() {
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	ImVec4 col_area = ImVec4(0.047f, 0.169f, 0.059f, 0.44f);
	ImVec4 col_main = ImVec4(0.2f, 0.627f, 0.169f, 0.86f);
	FILE *f = fopen("ux0:data/VitaDB/theme.ini", "r");
	if (f) {
		float values[4];
		char buffer[64];
		while (EOF != fscanf(f, "%[^=]=%f,%f,%f,%f\n", buffer, &values[0], &values[1], &values[2], &values[3])) {
			READ_FIRST_VAL(ChildWindowBg)
			READ_NEXT_VAL(FrameBg)
			READ_NEXT_VAL(FrameBgHovered)
			READ_NEXT_VAL(FrameBgActive)
			READ_NEXT_VAL(TitleBg)
			READ_NEXT_VAL(TitleBgCollapsed)
			READ_NEXT_VAL(TitleBgActive)
			READ_NEXT_VAL(MenuBarBg)
			READ_NEXT_VAL(ScrollbarBg)
			READ_NEXT_VAL(ScrollbarGrab)
			READ_NEXT_VAL(ScrollbarGrabHovered)
			READ_NEXT_VAL(ScrollbarGrabActive)
			READ_NEXT_VAL(CheckMark)
			READ_NEXT_VAL(SliderGrab)
			READ_NEXT_VAL(SliderGrabActive)
			READ_NEXT_VAL(Button)
			READ_NEXT_VAL(ButtonHovered)
			READ_NEXT_VAL(ButtonActive)
			READ_NEXT_VAL(Header)
			READ_NEXT_VAL(HeaderHovered)
			READ_NEXT_VAL(HeaderActive)
			READ_NEXT_VAL(ResizeGrip)
			READ_NEXT_VAL(ResizeGripHovered)
			READ_NEXT_VAL(ResizeGripActive)
			READ_NEXT_VAL(PlotLinesHovered)
			READ_NEXT_VAL(PlotHistogramHovered)
			READ_NEXT_VAL(TextSelectedBg)
			READ_NEXT_VAL(NavHighlight)
			READ_EXTRA_VAL(TextLabel)
			READ_EXTRA_VAL(TextOutdated)
			READ_EXTRA_VAL(TextUpdated)
		}
		fclose(f);
	} else { // Save default theme
		f = fopen("ux0:data/VitaDB/theme.ini", "w");
		WRITE_VAL(ChildWindowBg)
		WRITE_VAL(FrameBg)
		WRITE_VAL(FrameBgHovered)
		WRITE_VAL(FrameBgActive)
		WRITE_VAL(TitleBg)
		WRITE_VAL(TitleBgCollapsed)
		WRITE_VAL(TitleBgActive)
		WRITE_VAL(MenuBarBg)
		WRITE_VAL(ScrollbarBg)
		WRITE_VAL(ScrollbarGrab)
		WRITE_VAL(ScrollbarGrabHovered)
		WRITE_VAL(ScrollbarGrabActive)
		WRITE_VAL(CheckMark)
		WRITE_VAL(SliderGrab)
		WRITE_VAL(SliderGrabActive)
		WRITE_VAL(Button)
		WRITE_VAL(ButtonHovered)
		WRITE_VAL(ButtonActive)
		WRITE_VAL(Header)
		WRITE_VAL(HeaderHovered)
		WRITE_VAL(HeaderActive)
		WRITE_VAL(ResizeGrip)
		WRITE_VAL(ResizeGripHovered)
		WRITE_VAL(ResizeGripActive)
		WRITE_VAL(PlotLinesHovered)
		WRITE_VAL(PlotHistogramHovered)
		WRITE_VAL(TextSelectedBg)
		WRITE_VAL(NavHighlight)
		WRITE_EXTRA_VAL(TextLabel)
		WRITE_EXTRA_VAL(TextOutdated)
		WRITE_EXTRA_VAL(TextUpdated)
		fclose(f);
	}
}

int main(int argc, char *argv[]) {
	SceIoStat st1, st2;
	// Checking for libshacccg.suprx existence
	if (!(sceIoGetstat("ur0:/data/libshacccg.suprx", &st1) >= 0 || sceIoGetstat("ur0:/data/external/libshacccg.suprx", &st2) >= 0))
		early_fatal_error("Error: Runtime shader compiler (libshacccg.suprx) is not installed.");
	
	sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	sceIoMkdir("ux0:data/VitaDB", 0777);
	
	// Removing any failed app installation leftover
	if (sceIoGetstat("ux0:/data/VitaDB/vpk/eboot.bin", &st1) >= 0) {
		recursive_rmdir("ux0:/data/VitaDB/vpk");
	}
	
	// Initializing sceAppUtil
	SceAppUtilInitParam appUtilParam;
	SceAppUtilBootParam appUtilBootParam;
	memset(&appUtilParam, 0, sizeof(SceAppUtilInitParam));
	memset(&appUtilBootParam, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&appUtilParam, &appUtilBootParam);
	
	// Initializing sceCommonDialog
	SceCommonDialogConfigParam cmnDlgCfgParam;
	sceCommonDialogConfigParamInit(&cmnDlgCfgParam);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cmnDlgCfgParam.language);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cmnDlgCfgParam.enterButtonAssign);
	sceCommonDialogSetConfigParam(&cmnDlgCfgParam);
	
	// Initializing sceNet
	generic_mem_buffer = (uint8_t*)malloc(MEM_BUFFER_SIZE);
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	int ret = sceNetShowNetstat();
	SceNetInitParam initparam;
	if (ret == SCE_NET_ERROR_ENOTINIT) {
		initparam.memory = malloc(141 * 1024);
		initparam.size = 141 * 1024;
		initparam.flags = 0;
		sceNetInit(&initparam);
	}
	
	// Initializing vitaGL
	AppSelection *hovered = nullptr;
	AppSelection *to_download = nullptr;
	vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_NONE);
	LoadBackground();

	// Initializing dear ImGui
	ImGui::CreateContext();
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = 0;
	ImGui_ImplVitaGL_Init();
	ImGui::GetIO().Fonts->AddFontFromFileTTF("app0:/Roboto.ttf", 16.0f);
	set_gui_theme();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::GetIO().MouseDrawCursor = false;
	ImGui_ImplVitaGL_TouchUsage(false);
	ImGui_ImplVitaGL_GamepadUsage(true);
	ImGui_ImplVitaGL_MouseStickUsage(false);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, 0);
	
	// Start background audio playback
	SceUID thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
	sceKernelStartThread(thd, 0, NULL);
	
	// Downloading apps list
	thd = sceKernelCreateThread("Apps List Downloader", &appListThread, 0x10000100, 0x100000, 0, 0, NULL);
	sceKernelStartThread(thd, 0, NULL);
	do {
		DrawDownloaderDialog(downloader_pass, downloaded_bytes, total_bytes, "Downloading apps list", 1, true);
		res = sceKernelGetThreadInfo(thd, &info);
	} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
	sceAppMgrUmount("app0:");
	AppendAppDatabase("ux0:data/VitaDB/apps.json");
	
	// Downloading icons
	SceIoStat stat;
	bool needs_icons_pack = false;
	if (sceIoGetstat("ux0:data/VitaDB/icons", &stat) < 0) {
		needs_icons_pack = true;
	} else {
		time_t cur_time, last_time;
		cur_time = time(NULL);
		FILE *f = fopen("ux0:data/VitaDB/last_boot.txt", "r");
		if (f) {
			fscanf(f, "%ld", &last_time);
			fclose(f);
			//sceClibPrintf("cur time is %ld and last time is %ld\n", cur_time, last_time);
			if (cur_time - last_time > 2592000) {
				init_interactive_msg_dialog("A long time passed since your last visit. Do you want to download all apps icons in one go?");
				while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
					vglSwapBuffers(GL_TRUE);
				}
				// Workaround to prevent message dialog "burn in" on background
				for (int i = 0; i < 15; i++) {
					glClear(GL_COLOR_BUFFER_BIT);
					vglSwapBuffers(GL_FALSE);
				}
				SceMsgDialogResult msg_res;
				memset(&msg_res, 0, sizeof(SceMsgDialogResult));
				sceMsgDialogGetResult(&msg_res);
				sceMsgDialogTerm();
				if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
					needs_icons_pack = true;
				}
			}
		}
		f = fopen("ux0:data/VitaDB/last_boot.txt", "w");
		fprintf(f, "%ld", cur_time);
		fclose(f);
	}
	if (needs_icons_pack) {
		download_file("https://vitadb.rinnegatamante.it/icons_zip.php", "Downloading apps icons");
		sceIoMkdir("ux0:data/VitaDB/icons", 0777);
		extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/icons/");
		sceIoRemove(TEMP_DOWNLOAD_NAME);
	}
	
	//printf("start\n");
	char *changelog = nullptr;
	char ver_str[64];
	bool show_changelog = false;
	bool calculate_ver_len = true;
	bool go_to_top = false;
	bool fast_increment = false;
	bool fast_decrement = false;
	bool is_app_hovered;
	float ver_len = 0.0f;
	uint32_t oldpad;
	int filtered_entries;
	AppSelection *decrement_stack[4096];
	AppSelection *decremented_app = nullptr;
	int decrement_stack_idx = 0;
	while (!update_detected) {
		if (old_sort_idx != sort_idx) {
			old_sort_idx = sort_idx;
			sort_applist(apps);
		}
		
		if (bg_image || has_animated_bg)
			DrawBackground();
		
		ImGui_ImplVitaGL_NewFrame();
		
		if (ImGui::BeginMainMenuBar()) {
			char title[256];
			sprintf(title, "VitaDB Downloader - Currently listing %d results with '%s' filter", filtered_entries, filter_modes[filter_idx]);
			ImGui::Text(title);
			if (calculate_ver_len) {
				calculate_ver_len = false;
				sprintf(ver_str, "v.%s", VERSION);
				ImVec2 ver_sizes = ImGui::CalcTextSize(ver_str);
				ver_len = ver_sizes.x;
			}
			ImGui::SetCursorPosX(950 - ver_len);
			ImGui::Text(ver_str); 
			ImGui::EndMainMenuBar();
		}
		
		if (bg_image || has_animated_bg)
			ImGui::SetNextWindowBgAlpha(0.3f);
		ImGui::SetNextWindowPos(ImVec2(0, 21), ImGuiSetCond_Always);
		ImGui::SetNextWindowSize(ImVec2(553, 523), ImGuiSetCond_Always);
		ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
		
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Search: ");
		ImGui::SameLine();
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
		if (ImGui::Button(app_name_filter, ImVec2(-1.0f, 0.0f))) {
			init_interactive_ime_dialog("Insert search term", app_name_filter);
		}
		if (ImGui::IsItemHovered()) {
			hovered = nullptr;
			old_hovered = nullptr;
		}
		if (go_to_top) {
			ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
			ImGui::SetScrollHere();
			go_to_top = false;
			hovered = nullptr;
			old_hovered = nullptr;
		}
		ImGui::PopStyleVar();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Filter: ");
		ImGui::SameLine();
		ImGui::PushItemWidth(190.0f);
		if (ImGui::BeginCombo("##combo", filter_modes[filter_idx])) {
			for (int n = 0; n < sizeof(filter_modes) / sizeof(*filter_modes); n++) {
				bool is_selected = filter_idx == n;
				if (ImGui::Selectable(filter_modes[n], is_selected))
					filter_idx = n;
				if (is_selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			hovered = nullptr;
		ImGui::PopItemWidth();
		ImGui::AlignTextToFramePadding();
		ImGui::SameLine();
		ImGui::Spacing();
		ImGui::SameLine();
		ImGui::Text("Sort Mode: ");
		ImGui::SameLine();
		ImGui::PushItemWidth(-1.0f);
		if (ImGui::BeginCombo("##combo2", sort_modes_str[sort_idx])) {
			for (int n = 0; n < sizeof(sort_modes_str) / sizeof(*sort_modes_str); n++) {
				bool is_selected = sort_idx == n;
				if (ImGui::Selectable(sort_modes_str[n], is_selected))
					sort_idx = n;
				if (is_selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			hovered = nullptr;
		ImGui::PopItemWidth();
		ImGui::Separator();
		
		AppSelection *g = apps;
		filtered_entries = 0;
		int increment_idx = 0;
		is_app_hovered = false;
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
		while (g) {
			if (filterApps(g)) {
				g = g->next;
				continue;
			}
			if ((strlen(app_name_filter) == 0) || (strlen(app_name_filter) > 0 && (strcasestr(g->name, app_name_filter) || strcasestr(g->author, app_name_filter)))) {
				float y = ImGui::GetCursorPosY() + 2.0f;
				if (ImGui::Button(g->name, ImVec2(-1.0f, 0.0f))) {
					to_download = g;
				}
				if (ImGui::IsItemHovered()) {
					is_app_hovered = true;
					hovered = g;
					if (fast_increment)
						increment_idx = 1;
					else if (fast_decrement) {
						if (decrement_stack_idx == 0)
							fast_decrement = false;
						else
							decremented_app = decrement_stack[decrement_stack_idx >= 20 ? (decrement_stack_idx - 20) : 0];
					}
				} else if (increment_idx) {
					increment_idx++;
					ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
					ImGui::SetScrollHere();
					if (increment_idx == 21 || g->next == nullptr)
						increment_idx = 0;
				} else if (fast_decrement) {
					if (!decremented_app)
						decrement_stack[decrement_stack_idx++] = g;
					else if (decremented_app == g) {
						ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
						ImGui::SetScrollHere();
						fast_decrement = false;
					}	
				}
				ImVec2 tag_len;
				switch (g->state) {
				case APP_UNTRACKED:
					tag_len = ImGui::CalcTextSize("Not Installed");
					ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
					ImGui::Text("Not Installed");
					break;
				case APP_OUTDATED:
					tag_len = ImGui::CalcTextSize("Outdated");
					ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
					ImGui::TextColored(TextOutdated, "Outdated");
					break;
				case APP_UPDATED:
					tag_len = ImGui::CalcTextSize("Updated");
					ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
					ImGui::TextColored(TextUpdated, "Updated");
					break;
				default:
					tag_len = ImGui::CalcTextSize("Unknown");
					ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
					ImGui::Text("Unknown");
					break;
				}
				filtered_entries++;
			}
			g = g->next;
		}
		ImGui::PopStyleVar();
		if (decrement_stack_idx == filtered_entries || !is_app_hovered)
			fast_decrement = false;
		fast_increment = false;
		ImGui::End();
		
		if (bg_image || has_animated_bg)
			ImGui::SetNextWindowBgAlpha(0.3f);
		ImGui::SetNextWindowPos(ImVec2(553, 21), ImGuiSetCond_Always);
		ImGui::SetNextWindowSize(ImVec2(407, 523), ImGuiSetCond_Always);
		ImGui::Begin("Info Window", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
		if (hovered) {
			LoadPreview(hovered);
			ImGui::SetCursorPos(ImVec2(preview_x + PREVIEW_PADDING, preview_y + PREVIEW_PADDING));
			ImGui::Image((void*)preview_icon, ImVec2(preview_width, preview_height));
			ImGui::TextColored(TextLabel, "Description:");
			ImGui::TextWrapped(hovered->desc);
			ImGui::SetCursorPosY(6);
			ImGui::SetCursorPosX(140);
			ImGui::TextColored(TextLabel, "Last Update:");
			ImGui::SetCursorPosY(22);
			ImGui::SetCursorPosX(140);
			ImGui::Text(hovered->date);
			ImGui::SetCursorPosY(6);
			ImGui::SetCursorPosX(330);
			ImGui::TextColored(TextLabel, "Downloads:");
			ImGui::SetCursorPosY(22);
			ImGui::SetCursorPosX(330);
			ImGui::Text(hovered->downloads);
			ImGui::SetCursorPosY(38);
			ImGui::SetCursorPosX(140);
			ImGui::TextColored(TextLabel, "Category:");
			ImGui::SetCursorPosY(54);
			ImGui::SetCursorPosX(140);
			switch (hovered->type[0]) {
			case '1':
				ImGui::Text("Original Game");
				break;
			case '2':
				ImGui::Text("Game Port");
				break;
			case '4':
				ImGui::Text("Utility");
				break;		
			case '5':
				ImGui::Text("Emulator");
				break;
			default:
				ImGui::Text("Unknown");
				break;
			}
			ImGui::SetCursorPosY(70);
			ImGui::SetCursorPosX(140);
			ImGui::TextColored(TextLabel, "Author:");
			ImGui::SetCursorPosY(86);
			ImGui::SetCursorPosX(140);
			ImGui::Text(hovered->author);
			ImGui::SetCursorPosY(100);
			ImGui::SetCursorPosX(140);
			ImGui::TextColored(TextLabel, "Size:");
			ImGui::SetCursorPosY(116);
			ImGui::SetCursorPosX(140);
			char size_str[64];
			char *dummy;
			uint64_t sz;
			if (strlen(hovered->data_link) > 5) {
				sz = strtoull(hovered->size, &dummy, 10);
				uint64_t sz2 = strtoull(hovered->data_size, &dummy, 10);
				sprintf(size_str, "VPK: %.2f %s, Data: %.2f %s", format_size(sz), format_size_str(sz), format_size(sz2), format_size_str(sz2));
			} else {
				sz = strtoull(hovered->size, &dummy, 10);
				sprintf(size_str, "VPK: %.2f %s", format_size(sz), format_size_str(sz));
			}
			ImGui::Text(size_str);
			ImGui::SetCursorPosY(454);
			if (strlen(hovered->screenshots) > 5) {
				ImGui::TextColored(TextLabel, "Press Start to view screenshots");
			}
			ImGui::SetCursorPosY(470);
			ImGui::TextColored(TextLabel, "Press Select to view changelog");
		}
		ImGui::SetCursorPosY(486);
		ImGui::Text("Current sorting mode: %s", sort_modes_str[sort_idx]);
		ImGui::SetCursorPosY(502);
		uint64_t total_space = get_total_storage();
		uint64_t free_space = get_free_storage();
		ImGui::Text("Free storage: %.2f %s / %.2f %s", format_size(free_space), format_size_str(free_space), format_size(total_space), format_size_str(total_space));
		ImGui::End();
		
		if (show_screenshots == 2) {
			LoadScreenshot();
			ImGui::SetNextWindowPos(ImVec2(80, 55), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800, 472), ImGuiSetCond_Always);
			ImGui::Begin("Screenshots Viewer (Left/Right to change current screenshot)", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			ImGui::Image((void*)preview_shot, ImVec2(800 - 19, 453 - 19));
			ImGui::End();
		}
		
		if (show_changelog) {
			ImGui::SetNextWindowPos(ImVec2(80, 55), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800, 472), ImGuiSetCond_Always);
			ImGui::Begin("Changelog Lister (Select to close)", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
			ImGui::TextWrapped(changelog ? changelog : "- First Release.");
			ImGui::End();
		}
		
		glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x), static_cast<int>(ImGui::GetIO().DisplaySize.y));
		ImGui::Render();
		ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
		vglSwapBuffers(GL_FALSE);
		
		// Extra controls handling
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_LTRIGGER && !(oldpad & SCE_CTRL_LTRIGGER) && !show_screenshots && !show_changelog) {
			sort_idx -= 1;
			if (sort_idx < 0)
				sort_idx = (sizeof(sort_modes_str) / sizeof(sort_modes_str[0])) - 1;
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_RTRIGGER && !(oldpad & SCE_CTRL_RTRIGGER) && !show_screenshots && !show_changelog) {
			sort_idx = (sort_idx + 1) % (sizeof(sort_modes_str) / sizeof(sort_modes_str[0]));
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_START && !(oldpad & SCE_CTRL_START) && hovered && strlen(hovered->screenshots) > 5 && !show_changelog) {
			show_screenshots = show_screenshots ? 0 : 1;
		} else if (pad.buttons & SCE_CTRL_SELECT && !(oldpad & SCE_CTRL_SELECT) && hovered && !show_screenshots) {
			show_changelog = !show_changelog;
			if (show_changelog)
				changelog = GetChangelog("ux0:data/VitaDB/apps.json", hovered->id);
			else
				free(changelog);
		} else if (pad.buttons & SCE_CTRL_LEFT && !(oldpad & SCE_CTRL_LEFT) && !show_changelog) {
			if (show_screenshots)
				cur_ss_idx--;
			else {
				fast_decrement = true;
				decrement_stack_idx = 0;
				decremented_app = nullptr;
			}
		} else if (pad.buttons & SCE_CTRL_RIGHT && !(oldpad & SCE_CTRL_RIGHT) && !show_changelog) {
			if (show_screenshots)
				cur_ss_idx++;
			else
				fast_increment = true;
		} else if (pad.buttons & SCE_CTRL_CIRCLE && !show_screenshots && !show_changelog) {
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_TRIANGLE && !(oldpad & SCE_CTRL_TRIANGLE) && !show_screenshots && !show_changelog) {
			init_interactive_ime_dialog("Insert search term", app_name_filter);
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_SQUARE && !(oldpad & SCE_CTRL_SQUARE) && !show_screenshots && !show_changelog) {
			filter_idx = (filter_idx + 1) % (sizeof(filter_modes) / sizeof(*filter_modes));
			go_to_top = true;
		}
		oldpad = pad.buttons;
		
		// Queued app download
		if (to_download) {
			if (to_download->requirements) {
				uint8_t *scr_data = vglMalloc(960 * 544 * 4);
				glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, scr_data);
				if (!previous_frame)
					glGenTextures(1, &previous_frame);
				glBindTexture(GL_TEXTURE_2D, previous_frame);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 960, 544, 0, GL_RGBA, GL_UNSIGNED_BYTE, scr_data);
				vglFree(scr_data);
				init_msg_dialog("This homebrew has extra requirements in order to work properly:\n%s", to_download->requirements);
				while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
					vglSwapBuffers(GL_TRUE);
				}
				sceMsgDialogTerm();
				glMatrixMode(GL_PROJECTION);
				glLoadIdentity();
				glOrthof(0, 960, 544, 0, 0, 1);
				glMatrixMode(GL_MODELVIEW);
				glLoadIdentity();
				float vtx[4 * 2] = {
					  0, 544,
					960, 544,
					  0,   0,
					960,   0
				};
				float txcoord[4 * 2] = {
					  0,   0,
					  1,   0,
					  0,   1,
					  1,   1
				};
				// Workaround to prevent message dialog "burn in" on background
				for (int i = 0; i < 15; i++) {
					glEnableClientState(GL_VERTEX_ARRAY);
					glEnableClientState(GL_TEXTURE_COORD_ARRAY);
					glVertexPointer(2, GL_FLOAT, 0, vtx);
					glTexCoordPointer(2, GL_FLOAT, 0, txcoord);
					glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
					vglSwapBuffers(GL_FALSE);
				}
			}
			if (strlen(to_download->data_link) > 5) {
				if (!to_download->requirements) {
					uint8_t *scr_data = vglMalloc(960 * 544 * 4);
					glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, scr_data);
					if (!previous_frame)
						glGenTextures(1, &previous_frame);
					glBindTexture(GL_TEXTURE_2D, previous_frame);
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 960, 544, 0, GL_RGBA, GL_UNSIGNED_BYTE, scr_data);
					vglFree(scr_data);
				}
				init_interactive_msg_dialog("This homebrew also has data files. Do you wish to install them as well?");
				while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
					vglSwapBuffers(GL_TRUE);
				}
				glMatrixMode(GL_PROJECTION);
				glLoadIdentity();
				glOrthof(0, 960, 544, 0, 0, 1);
				glMatrixMode(GL_MODELVIEW);
				glLoadIdentity();
				float vtx[4 * 2] = {
					  0, 544,
					960, 544,
					  0,   0,
					960,   0
				};
				float txcoord[4 * 2] = {
					  0,   0,
					  1,   0,
					  0,   1,
					  1,   1
				};
				// Workaround to prevent message dialog "burn in" on background
				for (int i = 0; i < 15; i++) {
					glEnableClientState(GL_VERTEX_ARRAY);
					glEnableClientState(GL_TEXTURE_COORD_ARRAY);
					glVertexPointer(2, GL_FLOAT, 0, vtx);
					glTexCoordPointer(2, GL_FLOAT, 0, txcoord);
					glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
					vglSwapBuffers(GL_FALSE);
				}
				SceMsgDialogResult msg_res;
				memset(&msg_res, 0, sizeof(SceMsgDialogResult));
				sceMsgDialogGetResult(&msg_res);
				sceMsgDialogTerm();
				if (msg_res.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES) {
					// Check if enough storage is left for the install
					char *dummy;
					uint64_t sz = strtoull(to_download->size, &dummy, 10);
					uint64_t sz2 = strtoull(to_download->data_size, &dummy, 10);
					if (free_space < (sz + sz2) * 2) {
						init_msg_dialog("Not enough free storage to install this application. Installation aborted.");
						while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
							vglSwapBuffers(GL_TRUE);
						}
						to_download = nullptr;
						sceMsgDialogTerm();
						continue;
					}
					download_file(to_download->data_link, "Downloading data files");
					extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/");
					sceIoRemove(TEMP_DOWNLOAD_NAME);
				}
			}
			// Check if enough storage is left for the install
			char *dummy;
			uint64_t sz = strtoull(to_download->size, &dummy, 10);
			if (free_space < sz * 2) {
				init_msg_dialog("Not enough free storage to install this application. Installation aborted.");
				while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
					vglSwapBuffers(GL_TRUE);
				}
				sceMsgDialogTerm();
				to_download = nullptr;
				continue;
			}
			sprintf(download_link, "https://vitadb.rinnegatamante.it/get_hb_url.php?id=%s", to_download->id);
			download_file(download_link, "Downloading vpk");
			if (!strncmp(to_download->id, "877", 3)) { // Updating VitaDB Downloader
				extract_file(TEMP_DOWNLOAD_NAME, "ux0:app/VITADBDLD/");
				sceIoRemove(TEMP_DOWNLOAD_NAME);
				sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
			} else {
				sceIoMkdir("ux0:data/VitaDB/vpk", 0777);
				extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/vpk/");
				sceIoRemove(TEMP_DOWNLOAD_NAME);
				FILE *f = fopen("ux0:data/VitaDB/vpk/hash.vdb", "w");
				fwrite(to_download->hash, 1, 32, f);
				fclose(f);
				makeHeadBin("ux0:data/VitaDB/vpk");
				scePromoterUtilInit();
				scePromoterUtilityPromotePkg("ux0:data/VitaDB/vpk", 0);
				int state = 0;
				do {
					int ret = scePromoterUtilityGetState(&state);
					if (ret < 0)
						break;
					DrawTextDialog("Installing the app", true, false);
					vglSwapBuffers(GL_TRUE);
				} while (state);
				scePromoterUtilTerm();
				if (sceIoGetstat("ux0:/data/VitaDB/vpk/eboot.bin", &st1) >= 0) {
					init_msg_dialog("The installation process failed.");
					while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
						vglSwapBuffers(GL_TRUE);
					}
					sceMsgDialogTerm();
					recursive_rmdir("ux0:/data/VitaDB/vpk");
				} else
					to_download->state = APP_UPDATED;
				to_download = nullptr;
			}
		}
		
		// Queued icon download
		if (need_icon) {
			sprintf(download_link, "https://rinnegatamante.it/vitadb/icons/%s", old_hovered->icon);				
			download_file(download_link, "Downloading missing icon");
			sprintf(download_link, "ux0:data/VitaDB/icons/%s", old_hovered->icon);
			sceIoRename(TEMP_DOWNLOAD_NAME, download_link);
			old_hovered = nullptr;
			need_icon = false;
		}
		
		// Ime dialog active
		if (is_ime_active) {
			while (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
				vglSwapBuffers(GL_TRUE);
			}
			SceImeDialogResult res;
			sceClibMemset(&res, 0, sizeof(SceImeDialogResult));
			sceImeDialogGetResult(&res);
			if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
				getDialogTextResult(app_name_filter);
			}
			sceImeDialogTerm();
			is_ime_active = false;
		}
		
		// Queued screenshots download
		if (show_screenshots == 1) {
			sceIoRemove("ux0:data/VitaDB/ss0.png");
			sceIoRemove("ux0:data/VitaDB/ss1.png");
			sceIoRemove("ux0:data/VitaDB/ss2.png");
			sceIoRemove("ux0:data/VitaDB/ss3.png");
			old_ss_idx = -1;
			cur_ss_idx = 0;
			char *s = hovered->screenshots;
			int shot_idx = 0;
			for (;;) {
				char *end = strstr(s, ";");
				if (end) {
					end[0] = 0;
				}
				sprintf(download_link, "https://vitadb.rinnegatamante.it/%s", s);				
				download_file(download_link, "Downloading screenshot");
				sprintf(download_link, "ux0:data/VitaDB/ss%d.png", shot_idx++);
				sceIoRename(TEMP_DOWNLOAD_NAME, download_link);
				if (end) {
					end[0] = ';';
					s = end + 1;
				} else {
					break;
				}
			}
			show_screenshots = 2;
		}
	}
	
	// Installing update
	download_file("https://vitadb.rinnegatamante.it/get_hb_url.php?id=877", "Downloading update");
	extract_file(TEMP_DOWNLOAD_NAME, "ux0:app/VITADBDLD/");
	sceIoRemove(TEMP_DOWNLOAD_NAME);
	sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
}

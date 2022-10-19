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
#include <taihen.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include "unzip.h"
#include "player.h"
#include "promoter.h"
#include "utils.h"
#include "dialogs.h"
#include "network.h"
#include "md5.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define VERSION "1.6"

#define MIN(x, y) (x) < (y) ? (x) : (y)
#define PREVIEW_PADDING 6
#define PREVIEW_PADDING_THEME 60
#define PREVIEW_HEIGHT (mode_idx == MODE_THEMES ? 159.0f : (mode_idx == MODE_VITA_HBS ? 128.0f : 80.0f))
#define PREVIEW_WIDTH  (mode_idx == MODE_THEMES ? 280.0f : (mode_idx == MODE_VITA_HBS ? 128.0f : 144.0f))

int _newlib_heap_size_user = 200 * 1024 * 1024;
int filter_idx = 0;
int cur_ss_idx;
int old_ss_idx = -1;
static char download_link[512];
char app_name_filter[128] = {0};
SceUID audio_thd = -1;
void *audio_buffer = nullptr;
bool shuffle_themes = false;

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
	char aux_hash[34];
	char *requirements;
	char data_link[128];
	int state;
	AppSelection *next;
};

struct ThemeSelection {
	char name[192];
	char author[64];
	char *desc;
	char credits[128];
	char bg_type[2];
	char has_music[2];
	char has_font[2];
	int state;
	bool shuffle;
	ThemeSelection *next;
};

const char *modes[] = {
	"Vita Homebrews",
	"PSP Homebrews",
	"Themes"
};
enum{
	MODE_VITA_HBS,
	MODE_PSP_HBS,
	MODE_THEMES,
	MODES_NUM
};
int mode_idx = 0;

static AppSelection *old_hovered = NULL;
ThemeSelection *themes = nullptr;
AppSelection *apps = nullptr;
AppSelection *psp_apps = nullptr;
AppSelection *to_download = nullptr;

char *extractValue(char *dst, char *src, char *val, char **new_ptr) {
	char label[32];
	sprintf(label, "\"%s\": \"", val);
	//printf("label: %s\n", label);
	char *ptr = strstr(src, label) + strlen(label);
	//printf("ptr is: %X\n", ptr);
	if (ptr == strlen(label))
		return nullptr;
	char *end2 = strstr(ptr, (val[0] == 'l' || val[0] == 'c') ? "\"," : "\"");
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
				if (res)
					res = unescape(res);
				break;
			}
		} while (ptr);
		fclose(f);
		free(buffer);
	}
	return res;
}

char *aux_main_files[] = {
	"Media/sharedassets0.assets.resS", // Unity
	"games/game.win", // GameMaker Studio
	"index.lua", // LuaPlayer Plus Vita
	"game.apk", // YoYo Loader
	"game_data/game.pck" // Godot
};

enum {
	VITA_EXECUTABLE,
	PSP_EXECUTABLE,
	AUXILIARY_FILE
};

bool checksum_match(char *hash_fname, char *fname, AppSelection *node, uint8_t type) {
	char cur_hash[40], aux_fname[256];
	FILE *f2 = fopen(hash_fname, "r");
	if (f2) {
		fread(cur_hash, 1, 32, f2);
		cur_hash[32] = 0;
		fclose(f2);
		if (strncmp(cur_hash, type != AUXILIARY_FILE ? node->hash : node->aux_hash, 32))
			node->state = APP_OUTDATED;
		else
			node->state = APP_UPDATED;
		return true;
	} else {
		if (type != AUXILIARY_FILE)
			f2 = fopen(fname, "r");
		else {
			for (int i = 0; i < sizeof(aux_main_files) / sizeof(*aux_main_files); i++) {
				sprintf(aux_fname, "ux0:app/%s/%s", node->titleid, aux_main_files[i]);
				//printf("attempting with %s\n", aux_fname);
				f2 = fopen(aux_fname, "r");
				if (f2)
					break;
			}
		}
		if (f2) {
			MD5Context ctx;
			MD5Init(&ctx);
			fseek(f2, 0, SEEK_END);
			int file_size = ftell(f2);
			fseek(f2, 0, SEEK_SET);
			int read_size = 0;
			while (read_size < file_size) {
				int read_buf_size = (file_size - read_size) > MEM_BUFFER_SIZE ? MEM_BUFFER_SIZE : (file_size - read_size);
				fread(generic_mem_buffer, 1, read_buf_size, f2);
				read_size += read_buf_size;
				MD5Update(&ctx, generic_mem_buffer, read_buf_size);
			}
			fclose(f2);
			//printf("closing file to hash\n");
			unsigned char md5_buf[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
			MD5Final(md5_buf, &ctx);
			sprintf(cur_hash, "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
				md5_buf[0], md5_buf[1], md5_buf[2], md5_buf[3], md5_buf[4], md5_buf[5], md5_buf[6], md5_buf[7], md5_buf[8],
				md5_buf[9], md5_buf[10], md5_buf[11], md5_buf[12], md5_buf[13], md5_buf[14], md5_buf[15]);
			//printf("local hash %s\n", cur_hash);
			if (strncmp(cur_hash, type != AUXILIARY_FILE ? node->hash : node->aux_hash, 32))
				node->state = APP_OUTDATED;
			else
				node->state = APP_UPDATED;
			switch (type) {
			case VITA_EXECUTABLE:
				sprintf(aux_fname, "ux0:app/%s/hash.vdb", node->titleid);
				break;
			case PSP_EXECUTABLE:
				sprintf(aux_fname, "ux0:pspemu/PSP/GAME/%s/hash.vdb", node->id);
				break;
			case AUXILIARY_FILE:
				sprintf(aux_fname, "ux0:app/%s/aux_hash.vdb", node->titleid);
				break;
			default:
				printf("Fatal Error!!!!\n");
				break;
			}
			f2 = fopen(aux_fname, "w");
			fwrite(cur_hash, 1, 32, f2);
			fclose(f2);
			return true;
		} else
			node->state = APP_UNTRACKED;
		return false;
	}
}

bool update_detected = false;
void AppendAppDatabase(const char *file, bool is_psp) {
	// Read icons database
	FILE *f = fopen("ux0:data/VitaDB/icons.db", "r");
	//printf("f is %x\n", f);
	size_t icons_db_size = fread(generic_mem_buffer, 1, MEM_BUFFER_SIZE, f);
	//printf("icons_db_size is %x\n", icons_db_size);
	char *icons_db = (char *)vglMalloc(icons_db_size + 1);
	sceClibMemcpy(icons_db, generic_mem_buffer, icons_db_size);
	icons_db[icons_db_size] = 0;
	fclose(f);
	
	uint32_t missing_icons_num = 0;
	AppSelection *missing_icons[2048];

	// Burning on screen the parsing text dialog
	for (int i = 0; i < 3; i++) {
		DrawTextDialog("Parsing apps list", true, !is_psp);
	}
	f = fopen(file, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		uint64_t len = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		fread(buffer, 1, len, f);
		fclose(f);
		buffer[len] = 0;
		char *ptr = buffer;
		char *end, *end2;
		do {
			char name[128], version[64], fname[128], fname2[128];
			ptr = extractValue(name, ptr, "name", nullptr);
			//printf("parsing %s\n", name);
			if (!ptr)
				break;
			AppSelection *node = (AppSelection*)malloc(sizeof(AppSelection));
			node->desc = nullptr;
			node->requirements = nullptr;
			ptr = extractValue(node->icon, ptr, "icon", nullptr);
			if (!strstr(icons_db, node->icon)) {
				missing_icons[missing_icons_num++] = node;
				//printf("%s is missing [%s]\n", node->icon, name);
			}
			ptr = extractValue(version, ptr, "version", nullptr);
			ptr = extractValue(node->author, ptr, "author", nullptr);
			ptr = extractValue(node->type, ptr, "type", nullptr);
			ptr = extractValue(node->id, ptr, "id", nullptr);
			if (!strncmp(node->id, "877", 3)) { // VitaDB Downloader, check if newer than running version
				if (strncmp(&version[2], VERSION, 3)) {
					update_detected = true;
					to_download = node;
				}
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
			if (is_psp) {
				sprintf(fname, "ux0:pspemu/PSP/GAME/%s/hash.vdb", node->id);
				sprintf(fname2, "ux0:pspemu/PSP/GAME/%s/EBOOT.PBP", node->id);
			} else {
				ptr = extractValue(node->aux_hash, ptr, "hash2", nullptr);
				//printf("aux db hash %s\n", node->aux_hash);
				sprintf(fname, "ux0:app/%s/hash.vdb", node->titleid);
				sprintf(fname2, "ux0:app/%s/eboot.bin", node->titleid);
			}
			if (checksum_match(fname, fname2, node, is_psp ? PSP_EXECUTABLE : VITA_EXECUTABLE)) {
				if (!is_psp && strlen(node->aux_hash) > 0) {
					sprintf(fname, "ux0:app/%s/aux_hash.vdb", node->titleid);
					for (int i = 0; i < sizeof(aux_main_files) / sizeof(*aux_main_files); i++) {
						if (checksum_match(fname, NULL, node, AUXILIARY_FILE))
							break;
					}
				}
			}
			//printf("hash part done\n");
			ptr = extractValue(node->requirements, ptr, "requirements", &node->requirements);
			if (node->requirements)
				node->requirements = unescape(node->requirements);
			ptr = extractValue(node->data_link, ptr, "data", nullptr);
			sprintf(node->name, "%s %s", name, version);
			if (is_psp) {
				node->next = psp_apps;
				psp_apps = node;
			} else {
				node->next = apps;
				apps = node;
			}
		} while (ptr);
		free(buffer);
		
		// Downloading missing icons
		for (int i = 0; i < missing_icons_num; i++) {
			sprintf(download_link, "https://rinnegatamante.it/vitadb/icons/%s", missing_icons[i]->icon);
			download_file(download_link, "Downloading missing icons");
			sprintf(download_link, "ux0:data/VitaDB/icons/%c%c", missing_icons[i]->icon[0], missing_icons[i]->icon[1]);
			sceIoMkdir(download_link, 0777);
			sprintf(download_link, "ux0:data/VitaDB/icons/%c%c/%s", missing_icons[i]->icon[0], missing_icons[i]->icon[1], missing_icons[i]->icon);
			sceIoRename(TEMP_DOWNLOAD_NAME, download_link);
			f = fopen("ux0:data/VitaDB/icons.db", "a");
			fprintf(f, "%s\n", download_link);
			fclose(f);
		}
	}
	vglFree(icons_db);
	//printf("finished parsing\n");
}

void AppendThemeDatabase(const char *file) {
	sceIoMkdir("ux0:data/VitaDB/themes", 0777);
	// Burning on screen the parsing text dialog
	for (int i = 0; i < 3; i++) {
		DrawTextDialog("Parsing themes list", true, false);
	}
	FILE *f = fopen(file, "rb");
	if (f) {
		uint32_t missing_previews_num = 0;
		ThemeSelection *missing_previews[2048];
		
		fseek(f, 0, SEEK_END);
		uint64_t len = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		fread(buffer, 1, len, f);
		buffer[len] = 0;
		char *ptr = buffer;
		char *end, *end2;
		do {
			char name[128], fname[256];
			SceIoStat st1;
			ptr = extractValue(name, ptr, "name", nullptr);
			//printf("parsing %s\n", name);
			if (!ptr)
				break;
			ThemeSelection *node = (ThemeSelection*)malloc(sizeof(ThemeSelection));
			sprintf(fname, "ux0:data/VitaDB/previews/%s.png", name);
			if (sceIoGetstat(fname, &st1) < 0)
				missing_previews[missing_previews_num++] = node;
			node->desc = nullptr;
			node->shuffle = false;
			strcpy(node->name, name);
			sprintf(fname, "ux0:data/VitaDB/themes/%s/theme.ini", node->name);
			FILE *f2 = fopen(fname, "r");
			if (f2)
				node->state = APP_UPDATED;
			else
				node->state = APP_UNTRACKED;
			ptr = extractValue(node->author, ptr, "author", nullptr);
			//printf("%s\n", node->author);
			ptr = extractValue(node->desc, ptr, "description", &node->desc);
			//printf("%s\n", node->desc);
			ptr = extractValue(node->credits, ptr, "credits", nullptr);
			//printf("%s\n", node->credits);
			ptr = extractValue(node->bg_type, ptr, "bg_type", nullptr);
			//printf("%s\n", node->bg_type);
			ptr = extractValue(node->has_music, ptr, "has_music", nullptr);
			//printf("%s\n", node->has_music);
			ptr = extractValue(node->has_font, ptr, "has_font", nullptr);
			//printf("%s\n", node->has_font);
			node->next = themes;
			themes = node;
		} while (ptr);
		fclose(f);
		free(buffer);
		
		// Downloading missing previews
		for (int i = 0; i < missing_previews_num; i++) {
			sprintf(download_link, "https://github.com/CatoTheYounger97/vitaDB_themes/raw/main/previews/%s.png", missing_previews[i]->name);
			download_file(download_link, "Downloading missing previews");
			sprintf(download_link, "ux0:data/VitaDB/previews/%s.png", missing_previews[i]->name);
			sceIoRename(TEMP_DOWNLOAD_NAME, download_link);
		}
	}
	//printf("finished parsing\n");
}

static char fname[512], ext_fname[512], read_buffer[8192];

void early_extract_file(char *file, char *dir) {
	init_progressbar_dialog("Extracting SharkF00D"); // Hardcoded for now since it's the sole instance of this function
	FILE *f;
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
		sprintf(ext_fname, "%s/%s", dir, fname);
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
				sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
				sceMsgDialogProgressBarSetValue(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, ((zip_idx + 1) / num_files) * 100);
				vglSwapBuffers(GL_TRUE);
			}
			fclose(f);
			unzCloseCurrentFile(zipfile);
		}
		if ((zip_idx + 1) < num_files) unzGoToNextFile(zipfile);
	}
	unzClose(zipfile);
	sceMsgDialogClose();
	int status = sceMsgDialogGetStatus();
	do {
		vglSwapBuffers(GL_TRUE);
		status = sceMsgDialogGetStatus();
	} while (status != SCE_COMMON_DIALOG_STATUS_FINISHED);
	sceMsgDialogTerm();
}

void extract_file(char *file, char *dir, bool indexing) {
	FILE *f;
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
	FILE *f2;
	if (indexing)
		f2 = fopen("ux0:data/VitaDB/icons.db", "w");
	for (int zip_idx = 0; zip_idx < num_files; ++zip_idx) {
		unzGetCurrentFileInfo(zipfile, &file_info, fname, 512, NULL, 0, NULL, 0);
		if (indexing) {
			sprintf(ext_fname, "%s%c%c", dir, fname[0], fname[1]);
			sceIoMkdir(ext_fname, 0777);
			sprintf(ext_fname, "%s%c%c/%s", dir, fname[0], fname[1], fname);
		} else
			sprintf(ext_fname, "%s/%s", dir, fname);
		const size_t filename_length = strlen(ext_fname);
		if (ext_fname[filename_length - 1] != '/') {
			curr_file_bytes = 0;
			unzOpenCurrentFile(zipfile);
			recursive_mkdir(ext_fname);
			if (indexing)
				fprintf(f2, "%s\n", ext_fname);
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
	if (indexing)
		fclose(f2);
	unzClose(zipfile);
	ImGui::GetIO().MouseDrawCursor = false;
}

static int preview_width, preview_height, preview_x, preview_y;
GLuint preview_icon = 0, preview_shot = 0, previous_frame = 0, bg_image = 0;
int show_screenshots = 0; // 0 = off, 1 = download, 2 = show
void LoadPreview(AppSelection *game) {
	if (old_hovered == game)
		return;
	old_hovered = game;
	
	char banner_path[256];
	if (mode_idx == MODE_THEMES) {
		ThemeSelection *g = (ThemeSelection *)game;
		sprintf(banner_path, "ux0:data/VitaDB/previews/%s.png", g->name);
	} else
		sprintf(banner_path, "ux0:data/VitaDB/icons/%c%c/%s", game->icon[0], game->icon[1], game->icon);
	uint8_t *icon_data = stbi_load(banner_path, &preview_width, &preview_height, NULL, 4);
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
		has_animated_bg = false;
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

void swap_themes(ThemeSelection *a, ThemeSelection *b) {
	AppSelection tmp;
	
	// Swapping everything except next leaf pointer
	sceClibMemcpy(&tmp, a, sizeof(ThemeSelection) - 4);
	sceClibMemcpy(a, b, sizeof(ThemeSelection) - 4);
	sceClibMemcpy(b, &tmp, sizeof(ThemeSelection) - 4);
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
const char *sort_modes_themes_str[] = {
	"Alphabetical (A-Z)",
	"Alphabetical (Z-A)"
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
const char *filter_themes_modes[] = {
	"All Themes",
	"Downloaded Themes",
	"Not Downloaded Themes"
};
int sort_idx = 0;
int old_sort_idx = -1;

void sort_themelist(ThemeSelection *start) {
	// Checking for empty list
	if (start == NULL) 
		return; 
	
	int swapped; 
	ThemeSelection *ptr1; 
	ThemeSelection *lptr = NULL; 
  
	do { 
		swapped = 0; 
		ptr1 = start; 
		
		int64_t d1, d2;
		char * dummy;
		while (ptr1->next != lptr && ptr1->next) {
			switch (sort_idx) {
			case 0: // A-Z
				if (strcasecmp(ptr1->name, ptr1->next->name) > 0) {
					swap_themes(ptr1, ptr1->next); 
					swapped = 1; 
				}
				break;
			case 1: // Z-A
				if (strcasecmp(ptr1->name, ptr1->next->name) < 0) {
					swap_themes(ptr1, ptr1->next); 
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

bool filterThemes(ThemeSelection *p) {
	switch (filter_idx) {
	case 1:
		if (p->state != APP_UPDATED)
			return true;
		break;
	case 2:
		if (p->state != APP_UNTRACKED)
			return true;
		break;
	default:
		break;
	}
	return false;
}

volatile bool kill_audio_thread = false;
static int musicThread(unsigned int args, void *arg) {
	// Starting background music
	FILE *f = fopen("ux0:/data/VitaDB/bg.ogg", "r");
	int chn;
	Mix_Music *mus;
	if (f) {
		fseek(f, 0, SEEK_END);
		int size = ftell(f);
		fseek(f, 0, SEEK_SET);
		audio_buffer = vglMalloc(size);
		fread(audio_buffer, 1, size, f);
		fclose(f);
		SDL_RWops *rw = SDL_RWFromMem(audio_buffer, size);
		mus = Mix_LoadMUS_RW(rw, 0);
		Mix_PlayMusic(mus, -1);
	} else {
		return sceKernelExitDeleteThread(0);
	}
	while (!kill_audio_thread) {
		sceKernelDelayThread(1000);
	}
	Mix_HaltMusic();
	Mix_FreeMusic(mus);
	vglFree(audio_buffer);
	kill_audio_thread = false;
	return sceKernelExitDeleteThread(0);
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
ImVec4 TextNotInstalled = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
ImVec4 Shuffle = ImVec4(0.0f, 0.0f, 1.0f, 0.4f);
ImVec4 ShuffleHovered = ImVec4(0.0f, 0.0f, 1.0f, 1.0f);
ImVec4 TextShadow = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
uint32_t TextShadowU32 = 0x00000000;

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
			READ_FIRST_VAL(FrameBg)
			READ_NEXT_VAL(FrameBgHovered)
			READ_NEXT_VAL(TitleBgActive)
			READ_NEXT_VAL(MenuBarBg)
			READ_NEXT_VAL(ScrollbarBg)
			READ_NEXT_VAL(ScrollbarGrab)
			READ_NEXT_VAL(Button)
			READ_NEXT_VAL(ButtonHovered)
			READ_NEXT_VAL(Header)
			READ_NEXT_VAL(HeaderHovered)
			READ_NEXT_VAL(NavHighlight)
			READ_NEXT_VAL(Text)
			READ_NEXT_VAL(Separator)
			READ_NEXT_VAL(PlotHistogram)
			READ_NEXT_VAL(WindowBg)
			READ_NEXT_VAL(Border)
			READ_EXTRA_VAL(TextLabel)
			READ_EXTRA_VAL(TextOutdated)
			READ_EXTRA_VAL(TextUpdated)
			READ_EXTRA_VAL(TextNotInstalled)
			READ_EXTRA_VAL(Shuffle)
			READ_EXTRA_VAL(ShuffleHovered)
			READ_EXTRA_VAL(TextShadow)
		}
		fclose(f);
	} else { // Save default theme
		f = fopen("ux0:data/VitaDB/theme.ini", "w");
		WRITE_VAL(FrameBg)
		WRITE_VAL(FrameBgHovered)
		WRITE_VAL(TitleBgActive)
		WRITE_VAL(MenuBarBg)
		WRITE_VAL(ScrollbarBg)
		WRITE_VAL(ScrollbarGrab)
		WRITE_VAL(Button)
		WRITE_VAL(ButtonHovered)
		WRITE_VAL(Header)
		WRITE_VAL(HeaderHovered)
		WRITE_VAL(NavHighlight)
		WRITE_VAL(Text)
		WRITE_VAL(Separator)
		WRITE_VAL(PlotHistogram)
		WRITE_VAL(WindowBg)
		WRITE_VAL(Border)
		WRITE_EXTRA_VAL(TextLabel)
		WRITE_EXTRA_VAL(TextOutdated)
		WRITE_EXTRA_VAL(TextUpdated)
		WRITE_EXTRA_VAL(TextNotInstalled)
		WRITE_EXTRA_VAL(Shuffle)
		WRITE_EXTRA_VAL(ShuffleHovered)
		WRITE_EXTRA_VAL(TextShadow)
		fclose(f);
	}
	
	TextShadowU32 = (uint32_t)(TextShadow.x * 255.0f) | (uint32_t)(TextShadow.y * 255.0f) << 8 | (uint32_t)(TextShadow.z * 255.0f) << 16 | (uint32_t)(TextShadow.w * 255.0f) << 24;
	if (TextShadowU32)
		ImGui::PushFontShadow(TextShadowU32);
	else
		ImGui::PopFontShadow();
}

void install_theme(ThemeSelection *g) {
	char fname[256];
	// Burning on screen the parsing text dialog
	for (int i = 0; i < 3; i++) {
		DrawTextDialog("Installing theme", true, false);
	}
	
	// Deleting old theme files
	sceIoRemove("ux0:data/VitaDB/bg.png");
	sceIoRemove("ux0:data/VitaDB/bg.mp4");
	sceIoRemove("ux0:data/VitaDB/bg.ogg");
	sceIoRemove("ux0:data/VitaDB/font.ttf");
	
	// Kill old audio playback
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = sceKernelGetThreadInfo(audio_thd, &info);
	if (res >= 0) {
		kill_audio_thread = true;
		while (sceKernelGetThreadInfo(audio_thd, &info) >= 0) {
			sceKernelDelayThread(1000);
		}
	}

	//Start new background audio playback
	if (g->has_music[0] == '1') {
		sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.ogg", g->name);
		copy_file(fname, "ux0:data/VitaDB/bg.ogg");
		audio_thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
		sceKernelStartThread(audio_thd, 0, NULL);
	}
	
	// Kill old animated background
	if (has_animated_bg)
		video_close();
	
	// Load new background image
	switch (g->bg_type[0]) {
	case '1':
		sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.png", g->name);
		copy_file(fname, "ux0:data/VitaDB/bg.png");
		break;
	case '2':
		sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.mp4", g->name);
		copy_file(fname, "ux0:data/VitaDB/bg.mp4");
		break;
	default:
		break;
	}
	LoadBackground();
	
	// Set new color scheme
	sprintf(fname, "ux0:data/VitaDB/themes/%s/theme.ini", g->name);
	copy_file(fname, "ux0:data/VitaDB/theme.ini");
	set_gui_theme();
	
	// Set new font
	if (g->has_font[0] == '1') {
		sprintf(fname, "ux0:data/VitaDB/themes/%s/font.ttf", g->name);
		copy_file(fname, "ux0:data/VitaDB/font.ttf");
	}
	ImGui::GetIO().Fonts->Clear();
	ImGui_ImplVitaGL_InvalidateDeviceObjects();
	SceIoStat st1;
	if (sceIoGetstat("ux0:/data/VitaDB/font.ttf", &st1) >= 0)
		ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/data/VitaDB/font.ttf", 16.0f);
	else
		ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/app/VITADBDLD/Roboto.ttf", 16.0f);
}

void install_theme_from_shuffle(bool boot) {
	SceIoStat st1;
	int themes_num = 0;
	
	FILE *f = fopen("ux0:data/VitaDB/shuffle.cfg", "r");
	while (EOF != fscanf(f, "%[^\n]\n", &generic_mem_buffer[20 * 1024 * 1024 + 256 * themes_num])) {
		themes_num++;
	}
	fclose(f);
	
	int theme_id = rand() % themes_num;
	char *name = (char *)&generic_mem_buffer[20 * 1024 * 1024 + 256 * theme_id];
	//printf("name is %s\n", name);
	
	if (!boot) {
		for (int i = 0; i < 3; i++) {
			DrawTextDialog("Installing random theme", true, false);
		}
	}
	
	// Deleting old theme files
	sceIoRemove("ux0:data/VitaDB/bg.png");
	sceIoRemove("ux0:data/VitaDB/bg.mp4");
	sceIoRemove("ux0:data/VitaDB/bg.ogg");
	sceIoRemove("ux0:data/VitaDB/font.ttf");

	// Kill old audio playback
	if (!boot) {
		SceKernelThreadInfo info;
		info.size = sizeof(SceKernelThreadInfo);
		int res = sceKernelGetThreadInfo(audio_thd, &info);
		if (res >= 0) {
			kill_audio_thread = true;
			while (sceKernelGetThreadInfo(audio_thd, &info) >= 0) {
				sceKernelDelayThread(1000);
			}
		}
	}

	//Start new background audio playback
	sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.ogg", name);
	if (sceIoGetstat(fname, &st1) >= 0) {
		copy_file(fname, "ux0:data/VitaDB/bg.ogg");
		if (!boot) {
			audio_thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
			sceKernelStartThread(audio_thd, 0, NULL);
		}
	}

	// Kill old animated background
	if (has_animated_bg && !boot)
		video_close();

	// Load new background image
	sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.png", name);
	if (sceIoGetstat(fname, &st1) >= 0)
		copy_file(fname, "ux0:data/VitaDB/bg.png");
	else {
		sprintf(fname, "ux0:data/VitaDB/themes/%s/bg.mp4", name);
		if (sceIoGetstat(fname, &st1) >= 0)
			copy_file(fname, "ux0:data/VitaDB/bg.mp4");
	}
	if (!boot)
		LoadBackground();

	// Set new color scheme
	sprintf(fname, "ux0:data/VitaDB/themes/%s/theme.ini", name);
	copy_file(fname, "ux0:data/VitaDB/theme.ini");
	if (!boot)
		set_gui_theme();

	// Set new font
	sprintf(fname, "ux0:data/VitaDB/themes/%s/font.ttf", name);
	if (sceIoGetstat(fname, &st1) >= 0)
		copy_file(fname, "ux0:data/VitaDB/font.ttf");
	if (!boot) {
		ImGui::GetIO().Fonts->Clear();
		ImGui_ImplVitaGL_InvalidateDeviceObjects();
		if (sceIoGetstat("ux0:/data/VitaDB/font.ttf", &st1) >= 0)
			ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/data/VitaDB/font.ttf", 16.0f);
		else
			ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/app/VITADBDLD/Roboto.ttf", 16.0f);
	}
}

int main(int argc, char *argv[]) {
	srand(time(NULL));
	SceIoStat st1, st2;
	
	sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	sceIoMkdir("ux0:data/VitaDB", 0777);
	sceIoMkdir("ux0:pspemu", 0777);
	sceIoMkdir("ux0:pspemu/PSP", 0777);
	sceIoMkdir("ux0:pspemu/PSP/GAME", 0777);
	
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
	
	// Initializing sceCommonDialog
	SceCommonDialogConfigParam cmnDlgCfgParam;
	sceCommonDialogConfigParamInit(&cmnDlgCfgParam);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cmnDlgCfgParam.language);
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cmnDlgCfgParam.enterButtonAssign);
	sceCommonDialogSetConfigParam(&cmnDlgCfgParam);
	
	// Checking for network connection
	sceNetCtlInit();
	sceNetCtlInetGetState(&ret);
	if (ret != SCE_NETCTL_STATE_CONNECTED)
		early_fatal_error("Error: You need an Internet connection to run this application.");
	sceNetCtlTerm();
	
	// Checking for libshacccg.suprx existence
	bool use_ur0_config = false;
	char user_plugin_str[96];
	strcpy(user_plugin_str, "*SHARKF00D\nux0:data/vitadb.suprx\n*NPXS10031\nux0:data/vitadb.suprx\n");
	FILE *fp = fopen("ux0:tai/config.txt", "r");
	if (!fp) {
		fp = fopen("ur0:tai/config.txt", "r");
		use_ur0_config = true;
	}
	int cfg_size = fread(generic_mem_buffer, 1, MEM_BUFFER_SIZE, fp);
	fclose(fp);
	if (!strncmp(generic_mem_buffer, user_plugin_str, strlen(user_plugin_str))) {
		if (!(sceIoGetstat("ur0:/data/libshacccg.suprx", &st1) >= 0 || sceIoGetstat("ur0:/data/external/libshacccg.suprx", &st2) >= 0)) { // Step 2: Extract libshacccg.suprx
			sceIoRemove("ux0:/data/Runtime1.00.pkg");
			sceIoRemove("ux0:/data/Runtime2.01.pkg");
			early_download_file("https://vitadb.rinnegatamante.it/get_hb_url.php?id=567", "Downloading SharkF00D");
			sceIoMkdir("ux0:data/VitaDB/vpk", 0777);
			early_extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/vpk/");
			sceIoRemove(TEMP_DOWNLOAD_NAME);
			makeHeadBin("ux0:data/VitaDB/vpk");
			init_warning("Installing SharkF00D");
			scePromoterUtilInit();
			scePromoterUtilityPromotePkg("ux0:data/VitaDB/vpk", 0);
			int state = 0;
			do {
				vglSwapBuffers(GL_TRUE);
				int ret = scePromoterUtilityGetState(&state);
				if (ret < 0)
					break;
			} while (state);
			sceMsgDialogClose();
			int status = sceMsgDialogGetStatus();
			do {
				vglSwapBuffers(GL_TRUE);
				status = sceMsgDialogGetStatus();
			} while (status != SCE_COMMON_DIALOG_STATUS_FINISHED);
			sceMsgDialogTerm();
			scePromoterUtilTerm();
			sceAppMgrLaunchAppByName(0x60000, "SHARKF00D", "");
			sceKernelExitProcess(0);
		} else { // Step 3: Cleanup
			fp = fopen(use_ur0_config ? "ur0:tai/config.txt" : "ux0:tai/config.txt", "w");
			fwrite(&generic_mem_buffer[strlen(user_plugin_str)], 1, cfg_size - strlen(user_plugin_str), fp);
			fclose(fp);
			sceIoRemove("ux0:data/vitadb.skprx");
			sceIoRemove("ux0:data/vitadb.suprx");
#if 0 // On retail console, this causes the app to get minimized which we don't want to
			scePromoterUtilInit();
			scePromoterUtilityDeletePkg("SHARKF00D");
			int state = 0;
			do {
				int ret = scePromoterUtilityGetState(&state);
				if (ret < 0)
					break;
			} while (state);
			scePromoterUtilTerm();
#endif
		}
	} else if (!(sceIoGetstat("ur0:/data/libshacccg.suprx", &st1) >= 0 || sceIoGetstat("ur0:/data/external/libshacccg.suprx", &st2) >= 0)) { // Step 1: Download PSM Runtime and install it
		early_warning("Runtime shader compiler (libshacccg.suprx) is not installed. VitaDB Downloader will proceed with its extraction.");
		fp = fopen(use_ur0_config ? "ur0:tai/config.txt" : "ux0:tai/config.txt", "w");
		fwrite(user_plugin_str, 1, strlen(user_plugin_str), fp);
		fwrite(generic_mem_buffer, 1, cfg_size, fp);
		fclose(fp);
		early_download_file("http://ares.dl.playstation.net/psm-runtime/IP9100-PCSI00011_00-PSMRUNTIME000000.pkg", "Downloading PSM Runtime v.1.00");
		sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:/data/Runtime1.00.pkg");
		early_download_file("http://gs.ww.np.dl.playstation.net/ppkg/np/PCSI00011/PCSI00011_T8/286a65ec1ebc2d8b/IP9100-PCSI00011_00-PSMRUNTIME000000-A0201-V0100-e4708b1c1c71116c29632c23df590f68edbfc341-PE.pkg", "Downloading PSM Runtime v.2.01");
		sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:/data/Runtime2.01.pkg");
		copy_file("app0:vitadb.skprx", "ux0:data/vitadb.skprx");
		copy_file("app0:vitadb.suprx", "ux0:data/vitadb.suprx");
		taiLoadStartKernelModule("ux0:data/vitadb.skprx", 0, NULL, 0);
		sceAppMgrLaunchAppByName(0x60000, "NPXS10031", "[BATCH]host0:/package/Runtime1.00.pkg\nhost0:/package/Runtime2.01.pkg");
		sceKernelExitProcess(0);
	}
	
	// Initializing SDL and SDL mixer
	SDL_Init(SDL_INIT_AUDIO);
	Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 512);
	Mix_AllocateChannels(4);
	
	// Removing any failed app installation leftover
	if (sceIoGetstat("ux0:/data/VitaDB/vpk", &st1) >= 0)
		recursive_rmdir("ux0:/data/VitaDB/vpk");
	
	// Initializing sceAppUtil
	SceAppUtilInitParam appUtilParam;
	SceAppUtilBootParam appUtilBootParam;
	memset(&appUtilParam, 0, sizeof(SceAppUtilInitParam));
	memset(&appUtilBootParam, 0, sizeof(SceAppUtilBootParam));
	sceAppUtilInit(&appUtilParam, &appUtilBootParam);
	
	// Initializing vitaGL
	AppSelection *hovered = nullptr;
	vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_NONE);

	// Apply theme shuffling
	if (sceIoGetstat("ux0:/data/VitaDB/shuffle.cfg", &st1) >= 0)
		install_theme_from_shuffle(true);
	LoadBackground();

	// Initializing dear ImGui
	ImGui::CreateContext();
	SceKernelThreadInfo info;
	info.size = sizeof(SceKernelThreadInfo);
	int res = 0;
	ImGui_ImplVitaGL_Init_Extended();
	if (sceIoGetstat("ux0:/data/VitaDB/font.ttf", &st1) >= 0)
		ImGui::GetIO().Fonts->AddFontFromFileTTF("ux0:/data/VitaDB/font.ttf", 16.0f);
	else
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
	audio_thd = sceKernelCreateThread("Audio Playback", &musicThread, 0x10000100, 0x100000, 0, 0, NULL);
	sceKernelStartThread(audio_thd, 0, NULL);
	
	// Downloading icons
	if ((sceIoGetstat("ux0:/data/VitaDB/icons.db", &st1) < 0) || (sceIoGetstat("ux0:data/VitaDB/icons", &st2) < 0)) {
		download_file("https://vitadb.rinnegatamante.it/icons_zip.php", "Downloading apps icons");
		sceIoMkdir("ux0:data/VitaDB/icons", 0777);
		extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/icons/", true);
		sceIoRemove(TEMP_DOWNLOAD_NAME);
	} else if (sceIoGetstat("ux0:/data/VitaDB/icons/0b", &st1) < 0) {
		// Checking if old icons system is being used and upgrade it
		for (int i = 0; i < 3; i++) {
			DrawTextDialog("Upgrading icons system, please wait...", true, false);
		}
		recursive_rmdir("ux0:data/VitaDB/icons");
		download_file("https://vitadb.rinnegatamante.it/icons_zip.php", "Downloading apps icons");
		sceIoMkdir("ux0:data/VitaDB/icons", 0777);
		extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/icons/", true);
		sceIoRemove(TEMP_DOWNLOAD_NAME);
	}
	
	// Downloading apps list
	SceUID thd = sceKernelCreateThread("Apps List Downloader", &appListThread, 0x10000100, 0x100000, 0, 0, NULL);
	sceKernelStartThread(thd, 0, NULL);
	do {
		DrawDownloaderDialog(downloader_pass, downloaded_bytes, total_bytes, "Downloading apps list", 1, true);
		res = sceKernelGetThreadInfo(thd, &info);
	} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
	sceAppMgrUmount("app0:");
	AppendAppDatabase("ux0:data/VitaDB/apps.json", false);
	
	//printf("start\n");
	char *changelog = nullptr;
	char right_str[64];
	bool show_changelog = false;
	bool calculate_right_len = true;
	bool go_to_top = false;
	bool fast_increment = false;
	bool fast_decrement = false;
	bool is_app_hovered;
	float right_len = 0.0f;
	float text_diff_len = 0.0f;
	uint32_t oldpad;
	int filtered_entries;
	AppSelection *decrement_stack[4096];
	AppSelection *decremented_app = nullptr;
	ThemeSelection *to_install = nullptr;
	int decrement_stack_idx = 0;
	while (!update_detected) {
		if (old_sort_idx != sort_idx) {
			old_sort_idx = sort_idx;
			if (mode_idx == MODE_THEMES)
				sort_themelist(themes);
			else
				sort_applist(mode_idx == MODE_VITA_HBS ? apps : psp_apps);
		}
		
		if (bg_image || has_animated_bg)
			DrawBackground();
		
		ImGui_ImplVitaGL_NewFrame();
		
		if (ImGui::BeginMainMenuBar()) {
			char title[256];
			if (mode_idx == MODE_THEMES)
				sprintf(title, "VitaDB Downloader v.%s - Currently listing %d themes with '%s' filter", VERSION, filtered_entries, filter_themes_modes[filter_idx]);
			else
				sprintf(title, "VitaDB Downloader v.%s - Currently listing %d results with '%s' filter", VERSION, filtered_entries, filter_modes[filter_idx]);
			ImGui::Text(title);
			if (calculate_right_len) {
				calculate_right_len = false;
				sprintf(right_str, "%s", modes[mode_idx]);
				ImVec2 right_sizes = ImGui::CalcTextSize(right_str);
				right_len = right_sizes.x;
			}
			ImGui::SetCursorPosX(950 - right_len);
			ImGui::TextColored(TextLabel, right_str); 
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
		if (text_diff_len == 0.0f)
			text_diff_len = ImGui::CalcTextSize("Search: ").x - ImGui::CalcTextSize("Filter: ").x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + text_diff_len);
		ImGui::Text("Filter: ");
		ImGui::SameLine();
		ImGui::PushItemWidth(190.0f);
		if (mode_idx == MODE_THEMES) {
			if (ImGui::BeginCombo("##combo", filter_themes_modes[filter_idx])) {
				for (int n = 0; n < sizeof(filter_themes_modes) / sizeof(*filter_themes_modes); n++) {
					bool is_selected = filter_idx == n;
					if (ImGui::Selectable(filter_themes_modes[n], is_selected))
						filter_idx = n;
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		} else {
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
		}
		if (ImGui::IsItemHovered()) {
			hovered = nullptr;
			old_hovered = nullptr;
		}
		ImGui::PopItemWidth();
		ImGui::AlignTextToFramePadding();
		ImGui::SameLine();
		ImGui::Spacing();
		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
		ImGui::Text("Sort Mode: ");
		ImGui::SameLine();
		ImGui::PushItemWidth(-1.0f);
		if (mode_idx == MODE_THEMES) {
			if (ImGui::BeginCombo("##combo2", sort_modes_themes_str[sort_idx])) {
				for (int n = 0; n < sizeof(sort_modes_themes_str) / sizeof(*sort_modes_themes_str); n++) {
					bool is_selected = sort_idx == n;
					if (ImGui::Selectable(sort_modes_themes_str[n], is_selected))
						sort_idx = n;
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		} else {
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
		}
		if (ImGui::IsItemHovered()) {
			hovered = nullptr;
			old_hovered = nullptr;
		}
		ImGui::PopItemWidth();
		ImGui::Separator();
		
		if (mode_idx == MODE_THEMES) {
			ThemeSelection *g = themes;
			filtered_entries = 0;
			int increment_idx = 0;
			is_app_hovered = false;
			ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
			while (g) {
				if (filterThemes(g)) {
					g = g->next;
					continue;
				}
				if ((strlen(app_name_filter) == 0) || (strlen(app_name_filter) > 0 && (strcasestr(g->name, app_name_filter) || strcasestr(g->author, app_name_filter)))) {
					float y = ImGui::GetCursorPosY() + 2.0f;
					bool is_shuffle = false;
					if (g->shuffle) {
						is_shuffle = true;
						ImGui::PushStyleColor(ImGuiCol_Button, Shuffle);
						ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ShuffleHovered);
					}
					if (ImGui::Button(g->name, ImVec2(-1.0f, 0.0f))) {
						if (g->state == APP_UNTRACKED)
							to_download = (AppSelection *)g;
						else if (shuffle_themes)
							g->shuffle = !g->shuffle;
						else
							to_install = g;
					}
					if (is_shuffle) {
						ImGui::PopStyleColor(2);
					}
					if (ImGui::IsItemHovered()) {
						is_app_hovered = true;
						hovered = (AppSelection *)g;
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
							decrement_stack[decrement_stack_idx++] = (AppSelection *)g;
						else if (decremented_app == (AppSelection *)g) {
							ImGui::GetCurrentContext()->NavId = ImGui::GetCurrentContext()->CurrentWindow->DC.LastItemId;
							ImGui::SetScrollHere();
							fast_decrement = false;
						}	
					}
					ImVec2 tag_len;
					switch (g->state) {
					case APP_UNTRACKED:
						tag_len = ImGui::CalcTextSize("Not Downloaded");
						ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
						ImGui::TextColored(TextNotInstalled, "Not Downloaded");
						break;
					case APP_UPDATED:
						tag_len = ImGui::CalcTextSize("Downloaded");
						ImGui::SetCursorPos(ImVec2(520.0f - tag_len.x, y));
						ImGui::TextColored(TextUpdated, "Downloaded");
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
		} else {
			AppSelection *g = mode_idx == MODE_VITA_HBS ? apps : psp_apps;
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
						ImGui::TextColored(TextNotInstalled, "Not Installed");
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
			if (mode_idx == MODE_THEMES) {
				LoadPreview(hovered);
				ImGui::SetCursorPos(ImVec2(preview_x + PREVIEW_PADDING_THEME, preview_y + PREVIEW_PADDING));
				ImGui::Image((void*)preview_icon, ImVec2(preview_width, preview_height));
				ThemeSelection *node = (ThemeSelection *)hovered;
				ImGui::TextColored(TextLabel, "Author:");
				ImGui::TextWrapped(node->author);
				ImGui::TextColored(TextLabel, "Description:");
				ImGui::TextWrapped(node->desc);
				ImGui::TextColored(TextLabel, "Credits:");
				ImGui::TextWrapped(node->credits);
				ImGui::Separator();
				ImGui::TextColored(TextLabel, "Background Type: ");
				ImGui::SameLine();
				switch (node->bg_type[0]) {
				case '0':
					ImGui::Text("Static Color");
					break;
				case '1':
					ImGui::Text("Static Image");
					break;
				case '2':
					ImGui::Text("Animated Image");
					break;
				default:
					ImGui::Text("Unknown");
					break;
				}
				ImGui::TextColored(TextLabel, "Background Music: ");
				ImGui::SameLine();
				ImGui::Text(node->has_music[0] == '1' ? "Yes" : "No");
				ImGui::TextColored(TextLabel, "Custom Font: ");
				ImGui::SameLine();
				ImGui::Text(node->has_font[0] == '1' ? "Yes" : "No");
				ImGui::SetCursorPosY(438);
				ImGui::TextColored(TextLabel, "Press Start to view screenshots");
			} else {
				LoadPreview(hovered);
				ImGui::SetCursorPos(ImVec2(preview_x + PREVIEW_PADDING, preview_y + PREVIEW_PADDING));
				if (mode_idx == MODE_VITA_HBS) {
					ImGui::ImageRound((void*)preview_icon, ImVec2(preview_width, preview_height));
					ImGui::SetCursorPosY(100);
					ImGui::SetCursorPosX(140);
				} else
					ImGui::Image((void*)preview_icon, ImVec2(preview_width, preview_height));
				ImGui::TextColored(TextLabel, "Size:");
				if (mode_idx == MODE_VITA_HBS) {
					ImGui::SetCursorPosY(116);
					ImGui::SetCursorPosX(140);
				}
				char size_str[64];
				char *dummy;
				uint64_t sz;
				if (strlen(hovered->data_link) > 5) {
					sz = strtoull(hovered->size, &dummy, 10);
					uint64_t sz2 = strtoull(hovered->data_size, &dummy, 10);
					sprintf(size_str, "%s: %.2f %s, Data: %.2f %s", mode_idx == MODE_VITA_HBS ? "VPK" : "App", format_size(sz), format_size_str(sz), format_size(sz2), format_size_str(sz2));
				} else {
					sz = strtoull(hovered->size, &dummy, 10);
					sprintf(size_str, "%s: %.2f %s", mode_idx == MODE_VITA_HBS ? "VPK" : "App", format_size(sz), format_size_str(sz));
				}
				ImGui::Text(size_str);
				ImGui::TextColored(TextLabel, "Description:");
				ImGui::TextWrapped(hovered->desc);
				ImGui::SetCursorPosY(6);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::TextColored(TextLabel, "Last Update:");
				ImGui::SetCursorPosY(22);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::Text(hovered->date);
				ImGui::SetCursorPosY(6);
				ImGui::SetCursorPosX(330);
				ImGui::TextColored(TextLabel, "Downloads:");
				ImGui::SetCursorPosY(22);
				ImGui::SetCursorPosX(330);
				ImGui::Text(hovered->downloads);
				ImGui::SetCursorPosY(38);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::TextColored(TextLabel, "Category:");
				ImGui::SetCursorPosY(54);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
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
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::TextColored(TextLabel, "Author:");
				ImGui::SetCursorPosY(86);
				ImGui::SetCursorPosX(mode_idx == MODE_VITA_HBS ? 140 : 156);
				ImGui::Text(hovered->author);
				ImGui::SetCursorPosY(454);
				if (strlen(hovered->screenshots) > 5) {
					ImGui::TextColored(TextLabel, "Press Start to view screenshots");
				}
				ImGui::SetCursorPosY(470);
				ImGui::TextColored(TextLabel, "Press Select to view changelog");
			}
		}
		if (mode_idx == MODE_THEMES) {
			ImGui::SetCursorPosY(454);
			ImGui::TextColored(TextLabel, "Press Select to change themes mode");
			ImGui::SetCursorPosY(470);
			ImGui::Text("Current theme mode: %s", shuffle_themes ? "Shuffle" : "Single");
			ImGui::SetCursorPosY(486);
			ImGui::Text("Current sorting mode: %s", sort_modes_themes_str[sort_idx]);
		} else {
			ImGui::SetCursorPosY(486);
			ImGui::Text("Current sorting mode: %s", sort_modes_str[sort_idx]);
		}
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
			char titlebar[256];
			sprintf(titlebar, "%s Changelog (Select to close)", hovered->name);
			ImGui::SetNextWindowPos(ImVec2(80, 55), ImGuiSetCond_Always);
			ImGui::SetNextWindowSize(ImVec2(800, 472), ImGuiSetCond_Always);
			ImGui::Begin(titlebar, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
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
			calculate_right_len = true;
			old_sort_idx = -1;
			sort_idx = 0;
			filter_idx = 0;
			mode_idx = (mode_idx + 1) % MODES_NUM;
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_RTRIGGER && !(oldpad & SCE_CTRL_RTRIGGER) && !show_screenshots && !show_changelog) {
			if (mode_idx == MODE_THEMES)
				sort_idx = (sort_idx + 1) % (sizeof(sort_modes_themes_str) / sizeof(sort_modes_themes_str[0]));
			else
				sort_idx = (sort_idx + 1) % (sizeof(sort_modes_str) / sizeof(sort_modes_str[0]));
			go_to_top = true;
		} else if (pad.buttons & SCE_CTRL_START && !(oldpad & SCE_CTRL_START) && hovered && (strlen(hovered->screenshots) > 5 || mode_idx == MODE_THEMES) && !show_changelog) {
			show_screenshots = show_screenshots ? 0 : 1;
		} else if (pad.buttons & SCE_CTRL_SELECT && !(oldpad & SCE_CTRL_SELECT) && (hovered || mode_idx == MODE_THEMES) && !show_screenshots) {
			if (mode_idx == MODE_THEMES) {
				shuffle_themes = !shuffle_themes;
				ThemeSelection *g = themes;
				FILE *f = fopen("ux0:data/VitaDB/shuffle.cfg", "w");
				bool has_shuffle = false;
				while (g) {
					if (g->shuffle) {
						has_shuffle = true;
						fprintf(f, "%s\n", g->name);
						g->shuffle = false;
					}
					g = g->next;
				}
				fclose(f);
				if (has_shuffle) {
					install_theme_from_shuffle(false);
				} else
					sceIoRemove("ux0:data/VitaDB/shuffle.cfg");
			} else {
				show_changelog = !show_changelog;
				if (show_changelog)
					changelog = GetChangelog("ux0:data/VitaDB/apps.json", hovered->id);
				else
					free(changelog);
			}
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
			if (mode_idx == MODE_THEMES)
				filter_idx = (filter_idx + 1) % (sizeof(filter_themes_modes) / sizeof(*filter_themes_modes));
			else
				filter_idx = (filter_idx + 1) % (sizeof(filter_modes) / sizeof(*filter_modes));
			go_to_top = true;
		}
		oldpad = pad.buttons;
		
		// Queued app download
		if (to_download) {
			if (mode_idx == MODE_THEMES) {
				ThemeSelection *node = (ThemeSelection *)to_download;
				sprintf(download_link, "https://github.com/CatoTheYounger97/vitaDB_themes/blob/main/themes/%s/theme.zip?raw=true", node->name);
				download_file(download_link, "Downloading theme");
				sprintf(download_link, "ux0:data/VitaDB/themes/%s/", node->name);
				sceIoMkdir(download_link, 0777);
				extract_file(TEMP_DOWNLOAD_NAME, download_link, false);
				sceIoRemove(TEMP_DOWNLOAD_NAME);
				node->state = APP_UPDATED;
				to_download = nullptr;
			} else {
				if (to_download->requirements) {
					uint8_t *scr_data = (uint8_t *)vglMalloc(960 * 544 * 4);
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
						uint8_t *scr_data = (uint8_t *)vglMalloc(960 * 544 * 4);
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
						extract_file(TEMP_DOWNLOAD_NAME, mode_idx == MODE_VITA_HBS ? "ux0:data/" : "ux0:pspemu/", false);
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
				download_file(download_link, mode_idx == MODE_VITA_HBS ? "Downloading vpk" : "Downloading app");
				if (!strncmp(to_download->id, "877", 3)) { // Updating VitaDB Downloader
					extract_file(TEMP_DOWNLOAD_NAME, "ux0:app/VITADBDLD/", false);
					sceIoRemove(TEMP_DOWNLOAD_NAME);
					FILE *f = fopen("ux0:app/VITADBDLD/hash.vdb", "w");
					fwrite(to_download->hash, 1, 32, f);
					fclose(f);
					sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
				} else {
					FILE *f;
					char tmp_path[256];
					if (mode_idx == MODE_VITA_HBS) {
						sceIoMkdir("ux0:data/VitaDB/vpk", 0777);
						extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/vpk/", false);
						sceIoRemove(TEMP_DOWNLOAD_NAME);
						if (strlen(to_download->aux_hash) > 0) {
							f = fopen("ux0:data/VitaDB/vpk/aux_hash.vdb", "w");
							fwrite(to_download->aux_hash, 1, 32, f);
							fclose(f);
						}
						f = fopen("ux0:data/VitaDB/vpk/hash.vdb", "w");
					} else {
						sprintf(tmp_path, "ux0:pspemu/PSP/GAME/%s/", to_download->id);
						sceIoMkdir(tmp_path, 0777);
						extract_file(TEMP_DOWNLOAD_NAME, tmp_path, false);
						sceIoRemove(TEMP_DOWNLOAD_NAME);
						sprintf(tmp_path, "ux0:pspemu/PSP/GAME/%s/hash.vdb", to_download->id);
						f = fopen(tmp_path, "w");
					}
					fwrite(to_download->hash, 1, 32, f);
					fclose(f);
					if (mode_idx == MODE_VITA_HBS) {
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
						if (sceIoGetstat("ux0:/data/VitaDB/vpk", &st1) >= 0) {
							init_msg_dialog("The installation process failed.");
							while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
								vglSwapBuffers(GL_TRUE);
							}
							sceMsgDialogTerm();
							recursive_rmdir("ux0:/data/VitaDB/vpk");
						} else
							to_download->state = APP_UPDATED;
					} else
						to_download->state = APP_UPDATED;
					to_download = nullptr;
				}
			}
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
			int shot_idx = 0;
			if (mode_idx == MODE_THEMES) {
				ThemeSelection *node = (ThemeSelection *)hovered;
				sprintf(download_link, "https://github.com/CatoTheYounger97/vitaDB_themes/raw/main/themes/%s/preview.png", node->name);				
				download_file(download_link, "Downloading screenshot");
				sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/ss0.png");
			} else {
				char *s = hovered->screenshots;
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
			}
			show_screenshots = 2;
		}
		
		// Queued theme to install
		if (to_install) {
			sceIoRemove("ux0:data/VitaDB/shuffle.cfg");
			install_theme(to_install);
			to_install = nullptr;
		}
		
		// Queued themes database download
		if (mode_idx == MODE_THEMES && !themes) {
			if (sceIoGetstat("ux0:/data/VitaDB/previews", &st1) < 0) {
				download_file("https://github.com/CatoTheYounger97/vitaDB_themes/releases/download/Nightly/previews.zip", "Downloading themes previews");
				extract_file(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/", false);
			}
			
			download_file("https://github.com/CatoTheYounger97/vitaDB_themes/releases/download/Nightly/themes.json", "Downloading themes list");
			sceIoRemove("ux0:data/VitaDB/themes.json");
			sceIoRename(TEMP_DOWNLOAD_NAME, "ux0:data/VitaDB/themes.json");
			AppendThemeDatabase("ux0:data/VitaDB/themes.json");
		}
		
		// PSP database update required
		if (mode_idx == MODE_PSP_HBS && !psp_apps) {
			SceUID thd = sceKernelCreateThread("Apps List Downloader", &appPspListThread, 0x10000100, 0x100000, 0, 0, NULL);
			sceKernelStartThread(thd, 0, NULL);
			do {
				DrawDownloaderDialog(downloader_pass, downloaded_bytes, total_bytes, "Downloading PSP apps list", 1, true);
				res = sceKernelGetThreadInfo(thd, &info);
			} while (info.status <= SCE_THREAD_DORMANT && res >= 0);
			AppendAppDatabase("ux0:data/VitaDB/psp_apps.json", true);
		}
	}
	
	// Installing update
	download_file("https://vitadb.rinnegatamante.it/get_hb_url.php?id=877", "Downloading update");
	extract_file(TEMP_DOWNLOAD_NAME, "ux0:app/VITADBDLD/", false);
	sceIoRemove(TEMP_DOWNLOAD_NAME);
	FILE *f = fopen("ux0:app/VITADBDLD/hash.vdb", "w");
	fwrite(to_download->hash, 1, 32, f);
	fclose(f);
	sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
}

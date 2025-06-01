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
#include <malloc.h>
#include <vitasdk.h>
#include <vitaGL.h>
#include "database.h"
#include "dialogs.h"
#include "network.h"
#include "utils.h"

ThemeSelection *themes = nullptr;
AppSelection *apps = nullptr;
AppSelection *psp_apps = nullptr;
TrophySelection *trophies = nullptr;

static SceUID clash_thd;

extern char boot_params[1024];
extern AppSelection *to_download;

const char *sort_modes_apps_str[8] = {
	"Most Recent",
	"Oldest",
	"Most Downloaded",
	"Least Downloaded",
	"Alphabetical (A-Z)",
	"Alphabetical (Z-A)",
	"Smallest",
	"Largest"
};

const char *sort_modes_themes_str[2] = {
	"Alphabetical (A-Z)",
	"Alphabetical (Z-A)"
};

static const char *aux_main_files[5] = {
	"Media/sharedassets0.assets.resS", // Unity
	"games/game.win", // GameMaker Studio
	"index.lua", // LuaPlayer Plus Vita
	"game.apk", // YoYo Loader
	"game_data/game.pck" // Godot
};

static int clashThread(unsigned int args, void *arg) {
	AppSelection *app = apps;
	while (app) {
		AppSelection *chk = app->next;
		while (chk) {
			if (!strcmp(chk->titleid, app->titleid)) {
				app->next_clash = chk;
				chk->prev_clash = app;
				break;
			}
			chk = chk->next;
		}
		app = app->next;
	}
	//printf("clash thread ended\n");
	return sceKernelExitDeleteThread(0);
}

static char *get_value_from_json(char *dst, char *src, char *val, char **new_ptr) {
	char label[32];
	sprintf(label, "\"%s\": \"", val);
	//printf("label: %s\n", label);
	char *ptr = strstr(src, label) + strlen(label);
	//printf("ptr is: %X\n", ptr);
	if ((uintptr_t)ptr == strlen(label))
		return nullptr;
	char *end2 = strstr(ptr, (val[0] == 'l' || val[0] == 'c') ? "\"," : "\"");
	if (dst == nullptr) {
		if (end2 - ptr > 0) {
			dst = (char *)malloc(end2 - ptr + 1);
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

static bool checksum_match(char *hash_fname, char *fname, AppSelection *node, uint8_t type) {
	char cur_hash[40], aux_fname[256];
	SceUID f = sceIoOpen(hash_fname, SCE_O_RDONLY, 0777);
	if (f >= 0) {
		sceIoRead(f, cur_hash, 32);
		cur_hash[32] = 0;
		sceIoClose(f);
		if (strncmp(cur_hash, type != AUXILIARY_FILE ? node->hash : node->aux_hash, 32))
			node->state = APP_OUTDATED;
		else
			node->state = APP_UPDATED;
		return true;
	} else {
		if (type != AUXILIARY_FILE)
			f = sceIoOpen(fname, SCE_O_RDONLY, 0777);
		else {
			for (int i = 0; i < sizeof(aux_main_files) / sizeof(*aux_main_files); i++) {
				sprintf(aux_fname, "ux0:app/%s/%s", node->titleid, aux_main_files[i]);
				//printf("attempting with %s\n", aux_fname);
				f = sceIoOpen(aux_fname, SCE_O_RDONLY, 0777);
				if (f >= 0)
					break;
			}
		}
		if (f >= 0) {
			calculate_md5(f, cur_hash);
			if (strncmp(cur_hash, type != AUXILIARY_FILE ? node->hash : node->aux_hash, 32))
				node->state = APP_OUTDATED;
			else
				node->state = APP_UPDATED;
			switch (type) {
			case VITA_EXECUTABLE:
				sprintf(aux_fname, "ux0:app/%s/hash.vdb", node->titleid);
				break;
			case PSP_EXECUTABLE:
				sprintf(aux_fname, "%spspemu/PSP/GAME/%s/hash.vdb", pspemu_dev, node->id);
				break;
			case AUXILIARY_FILE:
				sprintf(aux_fname, "ux0:app/%s/aux_hash.vdb", node->titleid);
				break;
			default:
				printf("Fatal Error!!!!\n");
				break;
			}
			f = sceIoOpen(aux_fname, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
			sceIoWrite(f, cur_hash, 32);
			sceIoClose(f);
			return true;
		} else
			node->state = APP_UNTRACKED;
		return false;
	}
}

char *get_changelog(const char *file, char *id) {
	char *res = nullptr;
	SceUID f = sceIoOpen(file, SCE_O_RDONLY, 0777);
	if (f >= 0) {
		size_t len = sceIoLseek(f, 0, SCE_SEEK_END);
		sceIoLseek(f, 0, SCE_SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		sceIoRead(f, buffer, len);
		buffer[len] = 0;
		char *ptr = buffer;
		char *end, *end2;
		char cur_id[8];
		do {
			ptr = get_value_from_json(cur_id, ptr, "id", nullptr);
			if (!strncmp(cur_id, id, 3)) {
				ptr = get_value_from_json(res, ptr, "changelog", &res);
				if (res)
					res = unescape(res);
				break;
			}
		} while (ptr);
		sceIoClose(f);
		free(buffer);
	}
	return res;
}

void populate_apps_database(const char *file, bool is_psp) {
	// Read icons database
	SceUID f = sceIoOpen("ux0:data/VitaDB/icons.db", SCE_O_RDONLY, 0777);
	//printf("f is %x\n", f);
	size_t icons_db_size = sceIoRead(f, generic_mem_buffer, MEM_BUFFER_SIZE);
	//printf("icons_db_size is %x\n", icons_db_size);
	char *icons_db = (char *)vglMalloc(icons_db_size + 1);
	sceClibMemcpy(icons_db, generic_mem_buffer, icons_db_size);
	icons_db[icons_db_size] = 0;
	sceIoClose(f);
	
	uint32_t missing_icons_num = 0;
	AppSelection *missing_icons[2048];

	// Burning on screen the parsing text dialog
	for (int i = 0; i < 3; i++) {
		draw_text_dialog("Parsing apps list", true, !is_psp);
	}
	f = sceIoOpen(file, SCE_O_RDONLY, 0777);
	if (f >= 0) {
		size_t len = sceIoLseek(f, 0, SCE_SEEK_END);
		sceIoLseek(f, 0, SCE_SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		sceIoRead(f, buffer, len);
		sceIoClose(f);
		buffer[len] = 0;
		char *ptr = buffer;
		char *end, *end2;
		do {
			char name[128], version[64], fname[128], fname2[128], has_trophies[4];
			ptr = get_value_from_json(name, ptr, "name", nullptr);
			//printf("parsing %s\n", name);
			if (!ptr)
				break;
			AppSelection *node = (AppSelection*)malloc(sizeof(AppSelection));
			node->desc = nullptr;
			node->requirements = nullptr;
			node->next_clash = nullptr;
			node->prev_clash = nullptr;
			ptr = get_value_from_json(node->icon, ptr, "icon", nullptr);
			if (!strstr(icons_db, node->icon)) {
				missing_icons[missing_icons_num++] = node;
				//printf("%s is missing [%s]\n", node->icon, name);
			}
			ptr = get_value_from_json(version, ptr, "version", nullptr);
			ptr = get_value_from_json(node->author, ptr, "author", nullptr);
			ptr = get_value_from_json(node->type, ptr, "type", nullptr);
			ptr = get_value_from_json(node->id, ptr, "id", nullptr);
			if (!strncmp(node->id, "877", 3) && strlen(boot_params) == 0) { // VitaDB Downloader, check if newer than running version
				if (strncmp(&version[2], VERSION, 3)) {
					update_detected = true;
					to_download = node;
				}
			}
			ptr = get_value_from_json(node->date, ptr, "date", nullptr);
			ptr = get_value_from_json(node->titleid, ptr, "titleid", nullptr);
			ptr = get_value_from_json(node->screenshots, ptr, "screenshots", nullptr);
			ptr = get_value_from_json(node->desc, ptr, "long_description", &node->desc);
			node->desc = unescape(node->desc);
			ptr = get_value_from_json(node->downloads, ptr, "downloads", nullptr);
			ptr = get_value_from_json(node->source_page, ptr, "source", nullptr);
			ptr = get_value_from_json(node->release_page, ptr, "release_page", nullptr);
			ptr = get_value_from_json(node->trailer, ptr, "trailer", nullptr);
			ptr = get_value_from_json(node->size, ptr, "size", nullptr);
			ptr = get_value_from_json(node->data_size, ptr, "data_size", nullptr);
			ptr = get_value_from_json(node->hash, ptr, "hash", nullptr);
			//printf("db hash %s\n", node->hash);
			if (is_psp) {
				int type_num;
				sscanf(node->type, "%d", &type_num);
				type_num -= 10;
				sprintf(node->type, "%d", type_num);
				sprintf(fname, "%spspemu/PSP/GAME/%s/hash.vdb", pspemu_dev, node->id);
				sprintf(fname2, "%spspemu/PSP/GAME/%s/EBOOT.PBP", pspemu_dev, node->id);
			} else {
				ptr = get_value_from_json(node->aux_hash, ptr, "hash2", nullptr);
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
			ptr = get_value_from_json(node->requirements, ptr, "requirements", &node->requirements);
			if (node->requirements)
				node->requirements = unescape(node->requirements);
			ptr = get_value_from_json(has_trophies, ptr, "trophies", nullptr);
			node->trophies = atoi(has_trophies);
			ptr = get_value_from_json(node->data_link, ptr, "data", nullptr);
			if (strlen(node->data_link) > 5 && strstr(node->data_link, "www.rinnegatamante.eu")) { // Redirect to PSARCs any data files hosted on VitaDB webhost
				strcat(node->data_link, ".psarc");
			}
			sprintf(node->name, "%s %s", name, version);
			if (is_psp) {
				node->next = psp_apps;
				psp_apps = node;
			} else {
				if (node->state == APP_OUTDATED) {
					if (strlen(boot_params) > 0 && !strcmp(boot_params, node->id))
						to_download = node;
				}
				node->next = apps;
				apps = node;
			}
		} while (ptr);
		free(buffer);
		
		if (!is_psp) {
			// Populate TitleID clashes
			clash_thd = sceKernelCreateThread("Clasher Thread", &clashThread, 0x10000100, 0x100000, 0, 0, NULL);
			sceKernelStartThread(clash_thd, 0, NULL);
		}
		
		// Downloading missing icons
		if (!update_detected) {
			for (int i = 0; i < missing_icons_num; i++) {
				char download_link[512];
				sprintf(download_link, "https://www.rinnegatamante.eu/vitadb/icons/%s", missing_icons[i]->icon);
				download_file(download_link, "Downloading missing icons", false, i + 1, missing_icons_num);
				sprintf(download_link, "ux0:data/VitaDB/icons/%c%c", missing_icons[i]->icon[0], missing_icons[i]->icon[1]);
				sceIoMkdir(download_link, 0777);
				sprintf(download_link, "ux0:data/VitaDB/icons/%c%c/%s", missing_icons[i]->icon[0], missing_icons[i]->icon[1], missing_icons[i]->icon);
				sceIoRename(TEMP_DOWNLOAD_NAME, download_link);
				FILE *f = fopen("ux0:data/VitaDB/icons.db", "a");
				fprintf(f, "%s\n", download_link);
				fclose(f);
			}
		}
	}
	vglFree(icons_db);
	//printf("finished parsing\n");
}

static inline void swap_apps(AppSelection *prev, AppSelection *cur, AppSelection *next) {
	if (prev)
		prev->next = next;
	cur->next = next->next;
	next->next = cur;
}

void sort_apps_list(AppSelection **start, int sort_idx) {
	// Ensuring clasher titleids check finished
	sceKernelWaitThreadEnd(clash_thd, NULL, NULL);
	//printf("sort_apps_list called\n");

	// Checking for empty list
	if (start == NULL) 
		return; 
	
	bool swapped; 
  
	do {
		AppSelection *lptr = NULL; 
		AppSelection *ptr1 = *start; 
		swapped = false; 
		
		int64_t d1, d2;
		char *dummy;
		while (ptr1->next) {
			bool last_swapped = false;
			AppSelection *old_next = ptr1->next;
			switch (sort_idx) {
			case SORT_APPS_NEWEST:
				if (strcasecmp(ptr1->date, ptr1->next->date) < 0) {
					swap_apps(lptr, ptr1, ptr1->next); 
					swapped = true;
					last_swapped = true;
				}
				break;
			case SORT_APPS_OLDEST:
				if (strcasecmp(ptr1->date, ptr1->next->date) > 0) {
					swap_apps(lptr, ptr1, ptr1->next); 
					swapped = true;
					last_swapped = true;
				}
				break;
			case SORT_APPS_MOST_DOWNLOADED:
				d1 = strtoll(ptr1->downloads, &dummy, 10);
				d2 = strtoll(ptr1->next->downloads, &dummy, 10);
				if (d1 < d2) {
					swap_apps(lptr, ptr1, ptr1->next); 
					swapped = true;
					last_swapped = true;
				}
				break;
			case SORT_APPS_LEAST_DOWNLOADED:
				d1 = strtoll(ptr1->downloads, &dummy, 10);
				d2 = strtoll(ptr1->next->downloads, &dummy, 10);
				if (d1 > d2) {
					swap_apps(lptr, ptr1, ptr1->next); 
					swapped = 1;
					last_swapped = true;
				}
				break;
			case SORT_APPS_A_Z:
				if (strcasecmp(ptr1->name, ptr1->next->name) > 0) {
					swap_apps(lptr, ptr1, ptr1->next); 
					swapped = true;
					last_swapped = true;
				}
				break;
			case SORT_APPS_Z_A:
				if (strcasecmp(ptr1->name, ptr1->next->name) < 0) {
					swap_apps(lptr, ptr1, ptr1->next); 
					swapped = true;
					last_swapped = true;
				}
				break;
			case SORT_APPS_SMALLEST:
				d1 = strtoll(ptr1->size, &dummy, 10) + strtoll(ptr1->data_size, &dummy, 10);
				d2 = strtoll(ptr1->next->size, &dummy, 10) + strtoll(ptr1->next->data_size, &dummy, 10);
				if (d1 > d2) {
					swap_apps(lptr, ptr1, ptr1->next); 
					swapped = true;
					last_swapped = true;
				}
				break;
			case SORT_APPS_LARGEST:
				d1 = strtoll(ptr1->size, &dummy, 10) + strtoll(ptr1->data_size, &dummy, 10);
				d2 = strtoll(ptr1->next->size, &dummy, 10) + strtoll(ptr1->next->data_size, &dummy, 10);
				if (d1 < d2) {
					swap_apps(lptr, ptr1, ptr1->next); 
					swapped = true;
					last_swapped = true;
				}
				break;
			default:
				break;
			}
			if (!last_swapped) {
				lptr = ptr1;
				ptr1 = ptr1->next; 
			} else {
				if (*start == ptr1)
					*start = old_next;
				lptr = old_next;
			}
		} 
	} while (swapped); 
}

void populate_themes_database(const char *file) {
	sceIoMkdir("ux0:data/VitaDB/themes", 0777);
	// Burning on screen the parsing text dialog
	for (int i = 0; i < 3; i++) {
		draw_text_dialog("Parsing themes list", true, false);
	}
	SceUID f = sceIoOpen(file, SCE_O_RDONLY, 0777);
	if (f >= 0) {
		uint32_t missing_previews_num = 0;
		ThemeSelection *missing_previews[2048];
		
		size_t len = sceIoLseek(f, 0, SCE_SEEK_END);
		sceIoLseek(f, 0, SCE_SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		sceIoRead(f, buffer, len);
		buffer[len] = 0;
		char *ptr = buffer;
		char *end, *end2;
		do {
			char name[128], fname[256];
			SceIoStat st;
			ptr = get_value_from_json(name, ptr, "name", nullptr);
			//printf("parsing %s\n", name);
			if (!ptr)
				break;
			ThemeSelection *node = (ThemeSelection*)malloc(sizeof(ThemeSelection));
			sprintf(fname, "ux0:data/VitaDB/previews/%s.png", name);
			if (sceIoGetstat(fname, &st) < 0)
				missing_previews[missing_previews_num++] = node;
			node->desc = nullptr;
			node->shuffle = false;
			strcpy(node->name, name);
			sprintf(fname, "ux0:data/VitaDB/themes/%s/theme.ini", node->name);
			
			if (sceIoGetstat(fname, &st) >= 0)
				node->state = APP_UPDATED;
			else
				node->state = APP_UNTRACKED;
			ptr = get_value_from_json(node->author, ptr, "author", nullptr);
			//printf("%s\n", node->author);
			ptr = get_value_from_json(node->desc, ptr, "description", &node->desc);
			//printf("%s\n", node->desc);
			ptr = get_value_from_json(node->credits, ptr, "credits", nullptr);
			//printf("%s\n", node->credits);
			ptr = get_value_from_json(node->bg_type, ptr, "bg_type", nullptr);
			//printf("%s\n", node->bg_type);
			ptr = get_value_from_json(node->has_music, ptr, "has_music", nullptr);
			//printf("%s\n", node->has_music);
			ptr = get_value_from_json(node->has_font, ptr, "has_font", nullptr);
			//printf("%s\n", node->has_font);
			node->next = themes;
			themes = node;
		} while (ptr);
		sceIoClose(f);
		free(buffer);
		
		// Downloading missing previews
		for (int i = 0; i < missing_previews_num; i++) {
			char download_link[512];
			sprintf(download_link, "https://github.com/CatoTheYounger97/vitaDB_themes/raw/main/previews/%s.png", missing_previews[i]->name);
			download_file(download_link, "Downloading missing previews", false, i + 1, missing_previews_num);
			sprintf(download_link, "ux0:data/VitaDB/previews/%s.png", missing_previews[i]->name);
			sceIoRename(TEMP_DOWNLOAD_NAME, download_link);
		}
	}
	//printf("finished parsing\n");
}

static inline void swap_themes(ThemeSelection *prev, ThemeSelection *cur, ThemeSelection *next) {
	if (prev)
		prev->next = next;
	cur->next = next->next;
	next->next = cur;
}

void sort_themes_list(ThemeSelection **start, int sort_idx) {
	// Checking for empty list
	if (start == NULL) 
		return; 
	
	bool swapped; 
  
	do {
		ThemeSelection *ptr1 = *start; 
		ThemeSelection *lptr = NULL; 
		swapped = false; 

		int64_t d1, d2;
		while (ptr1->next) {
			bool last_swapped = false;
			ThemeSelection *old_next = ptr1->next;
			switch (sort_idx) {
			case SORT_THEMES_A_Z:
				if (strcasecmp(ptr1->name, ptr1->next->name) > 0) {
					swap_themes(lptr, ptr1, ptr1->next); 
					swapped = true;
					last_swapped = true;
				}
				break;
			case SORT_THEMES_Z_A:
				if (strcasecmp(ptr1->name, ptr1->next->name) < 0) {
					swap_themes(lptr, ptr1, ptr1->next); 
					swapped = true;
					last_swapped = true;
				}
				break;
			default:
				break;
			}
			if (!last_swapped) {
				lptr = ptr1;
				ptr1 = ptr1->next; 
			} else {
				if (*start == ptr1)
					*start = old_next;
				lptr = old_next;
			}
		} 
	} while (swapped); 
}
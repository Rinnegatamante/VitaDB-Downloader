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
#include <string.h>
#include <algorithm>
#include <cmath>
#include <vitasdk.h>
#include <vitaGL.h>
#include "database.h"
#include "robin_hood.h"
#include "dialogs.h"
#include "network.h"
#include "utils.h"

ThemeSelection *themes = nullptr;
AppSelection *apps = nullptr;
AppSelection *psp_apps = nullptr;
TrophySelection *trophies = nullptr;
std::vector<std::string> daemon_blacklist;
std::vector<std::string> favorites;
bool favorites_old_format = false;

char *hardcoded_daemon_blacklist[] = {
	"ABCD12345",
	"DEDALOX64",
	"RETROVITA",
	"JULIUS001",
	"DVLX00001",
	"GMSV00001",
	"MAIM00001",
	"MLCL00003",
	"OPENTITUS",
	"REGEDIT01",
	"SVMP00001",
	"SWKK00001",
	"VID000016",
	"VITAPONG0",
	"VSCU00001",
	"XASH00001"
};

extern char boot_params[1024];
extern AppSelection *to_download;

const char *sort_modes_apps_str[10] = {
	"Most Recent",
	"Oldest",
	"Most Downloaded",
	"Least Downloaded",
	"Alphabetical (A-Z)",
	"Alphabetical (Z-A)",
	"Smallest",
	"Largest",
	"Highest Game Score",
	"Lowest Game Score"
};

const char *sort_modes_themes_str[2] = {
	"Alphabetical (A-Z)",
	"Alphabetical (Z-A)"
};

static const char *aux_main_files[6] = {
	"Media/sharedassets0.assets.resS", // Unity
	"games/game.win", // GameMaker Studio
	"index.lua", // LuaPlayer Plus Vita
    "main.lua", // LifeLua
	"game.apk", // YoYo Loader
	"game_data/game.pck" // Godot
};

static void populate_titleid_clashes(AppSelection *head) {
	robin_hood::unordered_flat_map<std::string, AppSelection *> previous;
	for (AppSelection *app = head; app; app = app->next) {
		AppSelection *&prev = previous[app->titleid];
		app->next_clash = nullptr;
		app->prev_clash = prev;
		if (prev) {
			prev->next_clash = app;
		}
		prev = app;
	}
}

static char *get_value_from_json(char *dst, char *src, char *val, char **new_ptr) {
	char label[32];
	int label_len = sprintf(label, "\"%s\": \"", val);
	//printf("label: %s\n", label);
	char *ptr = strstr(src, label);
	if (!ptr) {
		return nullptr;
	}
	ptr += label_len;
	//printf("ptr is: %X\n", ptr);
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

static std::string filesystem_key(const char *name) {
	std::string key(name);
	for (char &c : key) {
		if (c >= 'A' && c <= 'Z') {
			c += 'a' - 'A';
		}
	}
	return key;
}

class InstalledDirectories {
	robin_hood::unordered_flat_set<std::string> names;
	bool complete = false;
public:
	explicit InstalledDirectories(const char *path) {
		SceUID dir = sceIoDopen(path);
		if (dir < 0) {
			return;
		}
		int result;
		do {
			SceIoDirent entry;
			result = sceIoDread(dir, &entry);
			if (result > 0 && SCE_S_ISDIR(entry.d_stat.st_mode)) {
				names.insert(filesystem_key(entry.d_name));
			}
		} while (result > 0);
		complete = result == 0;
		sceIoDclose(dir);
	}

	bool may_contain(const char *name) const {
		return !complete || names.count(filesystem_key(name)) != 0;
	}
};

struct CachedChecksum {
	bool present = false;
	char hash[33] = {};
};

using ChecksumCache = robin_hood::unordered_flat_map<std::string, CachedChecksum>;

static bool checksum_match(const char *hash_fname, const char *fname, AppSelection *node,
	uint8_t type, ChecksumCache &cache) {
	auto insertion = cache.emplace(filesystem_key(hash_fname), CachedChecksum());
	CachedChecksum &actual = insertion.first->second;
	if (insertion.second) {
		SceUID f = sceIoOpen(hash_fname, SCE_O_RDONLY, 0777);
		if (f >= 0) {
			int bytes = sceIoRead(f, actual.hash, 32);
			sceIoClose(f);
			actual.present = bytes == 32;
		}
		if (!actual.present) {
			if (type != AUXILIARY_FILE) {
				f = sceIoOpen(fname, SCE_O_RDONLY, 0777);
			} else {
				char aux_fname[256];
				for (const char *candidate : aux_main_files) {
					sprintf(aux_fname, "ux0:app/%s/%s", node->titleid, candidate);
					f = sceIoOpen(aux_fname, SCE_O_RDONLY, 0777);
					if (f >= 0) {
						break;
					}
				}
			}
			if (f >= 0) {
				calculate_md5(f, actual.hash);
				actual.present = true;
				SceUID hash_file = sceIoOpen(hash_fname, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
				if (hash_file >= 0) {
					sceIoWrite(hash_file, actual.hash, 32);
					sceIoClose(hash_file);
				}
			}
		}
	}
	const char *expected = type == AUXILIARY_FILE ? node->aux_hash : node->hash;
	node->state = !actual.present ? APP_UNTRACKED :
		(strncmp(actual.hash, expected, 32) ? APP_OUTDATED : APP_UPDATED);
	return actual.present;
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
				if (res) {
					res = unescape(res);
				}
				break;
			}
		} while (ptr);
		sceIoClose(f);
		free(buffer);
	}
	return res;
}

static void read_icon_index(robin_hood::unordered_flat_set<std::string> &icons) {
	SceUID f = sceIoOpen("ux0:data/VitaDB/icons.db", SCE_O_RDONLY, 0777);
	if (f < 0) {
		return;
	}
	int size = sceIoRead(f, generic_mem_buffer, MEM_BUFFER_SIZE - 1);
	sceIoClose(f);
	if (size <= 0) {
		return;
	}
	char *icons_text = (char *)generic_mem_buffer;
	icons_text[size] = 0;
	for (char *line = icons_text; line;) {
		char *end = strchr(line, '\n');
		if (end) {
			*end = 0;
		}
		size_t length = strlen(line);
		if (length && line[length - 1] == '\r') {
			line[length - 1] = 0;
		}
		char *slash = strrchr(line, '/');
		const char *name = slash ? slash + 1 : line;
		if (*name) {
			icons.insert(name);
		}
		line = end ? end + 1 : nullptr;
	}
}

bool populate_apps_database(const char *file, bool is_psp) {
	// Read icons database
	robin_hood::unordered_flat_set<std::string> indexed_icons;
	read_icon_index(indexed_icons);
	std::vector<AppSelection *> missing_icons;

	// Burning on screen the parsing text dialog
	for (int i = 0; i < 3; i++) {
		draw_text_dialog("Parsing apps list", true, !is_psp);
	}
	SceUID f = sceIoOpen(file, SCE_O_RDONLY, 0777);
	if (f >= 0) {
		size_t len = sceIoLseek(f, 0, SCE_SEEK_END);
		sceIoLseek(f, 0, SCE_SEEK_SET);
		char *buffer = (char*)malloc(len + 1);
		sceIoRead(f, buffer, len);
		sceIoClose(f);
		buffer[len] = 0;
		if (!strstr(buffer, "\"name\":")) {
			return false;
		}
		char *ptr = buffer;
		char *end, *end2;
		std::vector<std::string> new_favorites;
		std::string installed_path = is_psp ? std::string(pspemu_dev) + "pspemu/PSP/GAME" : "ux0:app";
		InstalledDirectories installed(installed_path.c_str());
		ChecksumCache checksums;
		robin_hood::unordered_flat_set<int> favorite_ids;
		robin_hood::unordered_flat_set<std::string> favorite_titles;
		for (const std::string &favorite : favorites) {
			if (favorites_old_format) {
				favorite_titles.insert(favorite);
			} else {
				favorite_ids.insert(atoi(favorite.c_str()));
			}
		}
		robin_hood::unordered_flat_set<std::string> blacklist;
		robin_hood::unordered_flat_set<std::string> hard_blacklist;
		if (!is_psp) {
			for (const std::string &title_id : daemon_blacklist) {
				blacklist.insert(title_id);
			}
			for (const char *title_id : hardcoded_daemon_blacklist) {
				hard_blacklist.insert(title_id);
			}
		}
		do {
			char name[128], version[64], fname[128], fname2[128], smalldata[4];
			ptr = get_value_from_json(name, ptr, "name", nullptr);
			//printf("parsing %s\n", name);
			if (!ptr) {
				break;
			}
			AppSelection *node = (AppSelection*)malloc(sizeof(AppSelection));
			node->search_filtered = false;
			node->filtered = false;
			node->desc = nullptr;
			node->requirements = nullptr;
			node->next_clash = nullptr;
			node->prev_clash = nullptr;
			node->state = APP_UNTRACKED;
			node->score = 0.0f;
			ptr = get_value_from_json(node->icon, ptr, "icon", nullptr);
			if (node->icon[0] && node->icon[1] && indexed_icons.insert(node->icon).second) {
				missing_icons.push_back(node);
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
				node->favorites = !favorites_old_format && favorite_ids.count(atoi(node->id)) != 0;
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
				
				node->blacklisted = hard_blacklist.count(node->titleid) ? APP_HARD_BLACKLISTED : (blacklist.count(node->titleid) ? APP_BLACKLISTED : APP_WHITELISTED);
				node->favorites = favorites_old_format ? favorite_titles.count(node->titleid) != 0 : favorite_ids.count(atoi(node->id)) != 0;
				if (node->favorites && favorites_old_format) {
					char padded_id[5];
					sprintf(padded_id, "%04d", atoi(node->id));
					new_favorites.push_back(padded_id);
				}
			}
			if (installed.may_contain(is_psp ? node->id : node->titleid) && checksum_match(fname, fname2, node, is_psp ? PSP_EXECUTABLE : VITA_EXECUTABLE, checksums)) {
				if (!is_psp && strlen(node->aux_hash) > 0) {
					sprintf(fname, "ux0:app/%s/aux_hash.vdb", node->titleid);
					checksum_match(fname, NULL, node, AUXILIARY_FILE, checksums);
				}
			}
			//printf("hash part done\n");
			ptr = get_value_from_json(node->requirements, ptr, "requirements", &node->requirements);
			if (node->requirements) {
				node->requirements = unescape(node->requirements);
			}
			ptr = get_value_from_json(smalldata, ptr, "trophies", nullptr);
			node->trophies = atoi(smalldata);
			ptr = get_value_from_json(smalldata, ptr, "ai", nullptr);
			node->ai = atoi(smalldata);
			if (!is_psp) {
				ptr = get_value_from_json(smalldata, ptr, "score", nullptr);
				node->score = atof(smalldata);
			}
			ptr = get_value_from_json(fname, ptr, "data", nullptr);
			node->has_data = strlen(fname) > 5;
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
		
		if (favorites_old_format) {
			std::string serialized = ".";
			for (const std::string &favorite : new_favorites) {
				if (serialized.size() > 1) {
					serialized += ';';
				}
				serialized += favorite;
			}
			SceUID fd = sceIoOpen("ux0:data/VitaDB/favorites.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
			sceIoWrite(fd, serialized.data(), serialized.size());
			sceIoClose(fd);
			favorites_old_format = false;
			favorites = new_favorites;
		}
		
		if (!is_psp) {
			// Populate TitleID clashes
			populate_titleid_clashes(apps);
		}
		
		// Downloading missing icons
		if (!update_detected && !missing_icons.empty()) {
			FILE *index = fopen("ux0:data/VitaDB/icons.db", "a");
			for (size_t i = 0; i < missing_icons.size(); i++) {
				char download_link[512];
				sprintf(download_link, "https://www.rinnegatamante.eu/vitadb/icons/%s", missing_icons[i]->icon);
				sceIoRemove(TEMP_DOWNLOAD_NAME);
				download_file(download_link, "Downloading missing icons", false, i + 1, missing_icons.size());
				sprintf(download_link, "ux0:data/VitaDB/icons/%c%c", missing_icons[i]->icon[0], missing_icons[i]->icon[1]);
				sceIoMkdir(download_link, 0777);
				sprintf(download_link, "ux0:data/VitaDB/icons/%c%c/%s", missing_icons[i]->icon[0], missing_icons[i]->icon[1], missing_icons[i]->icon);
				if (sceIoRename(TEMP_DOWNLOAD_NAME, download_link) >= 0 && index) {
					fprintf(index, "%s\n", download_link);
				}
			}
			if (index) {
				fclose(index);
			}
		}
	}
	//printf("finished parsing\n");
	return true;
}

void populate_daemon_blacklist() {
	daemon_blacklist.clear();
	SceUID fd = sceIoOpen("ux0:data/VitaDB/daemon_blacklist.txt", SCE_O_RDONLY, 0777);
	if (fd >= 0) {
		uint64_t len = sceIoLseek(fd, 0, SCE_SEEK_END);
		sceIoLseek(fd, 0, SCE_SEEK_SET);
		char *buffer = (char *)malloc(len + 1);
		char *_buffer = buffer;
		sceIoRead(fd, buffer, len);
		buffer[len] = 0;
		sceIoClose(fd);
		for (int i = 0; i < len; i += 10) {
			buffer[9] = 0;
			daemon_blacklist.push_back(buffer);
			buffer += 10;
		}
		free(_buffer);
	}
}

void insert_daemon_blacklist(char *tid) {
	daemon_blacklist.push_back(tid);
	SceUID fd = sceIoOpen("ux0:data/VitaDB/daemon_blacklist.txt", SCE_O_WRONLY | SCE_O_CREAT, 0777);
	uint64_t len = sceIoLseek(fd, 0, SCE_SEEK_END);
	if (len > 0) {
		char buffer[12];
		sprintf(buffer, ";%s", tid);
		sceIoWrite(fd, buffer, 10);
	} else {
		sceIoWrite(fd, tid, 9);
	}
	sceIoClose(fd);
}

void remove_daemon_blacklist(char *tid) {
	if (daemon_blacklist.size() > 1) {
		char *buffer = (char *)malloc(daemon_blacklist.size() * 10 + 1);
		buffer[0] = 0;
		int idx = 0;
		int to_delete = 0;
		for (std::string& s : daemon_blacklist) {
			if (s == tid) {
				to_delete = idx;
			} else {
				strcat(buffer, s.c_str());
				strcat(buffer, ";");
			}
			idx++;
		}
		daemon_blacklist.erase(daemon_blacklist.begin() + to_delete);
		SceUID fd = sceIoOpen("ux0:data/VitaDB/daemon_blacklist.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
		sceIoWrite(fd, buffer, daemon_blacklist.size() * 10 - 1);
		sceIoClose(fd);
		free(buffer);
	} else {
		daemon_blacklist.clear();
		sceIoRemove("ux0:data/VitaDB/daemon_blacklist.txt");
	}
}

void populate_favorites() {
	favorites.clear();
	SceUID fd = sceIoOpen("ux0:data/VitaDB/favorites.txt", SCE_O_RDONLY, 0777);
	if (fd >= 0) {
		char is_old;
		sceIoRead(fd, &is_old, 1);
		if (is_old == '.') {
			uint64_t len = sceIoLseek(fd, 0, SCE_SEEK_END);
			sceIoLseek(fd, 1, SCE_SEEK_SET);
			char *buffer = (char *)malloc(len + 1);
			char *_buffer = buffer;
			sceIoRead(fd, buffer, len);
			buffer[len] = 0;
			sceIoClose(fd);
			for (int i = 0; i < len; i += 5) {
				buffer[4] = 0;
				favorites.push_back(buffer);
				buffer += 5;
			}
			free(_buffer);
		} else {
			// Old format: needs to convert TitleIDs to MariaDB IDs
			favorites_old_format = true;
			uint64_t len = sceIoLseek(fd, 0, SCE_SEEK_END);
			sceIoLseek(fd, 0, SCE_SEEK_SET);
			char *buffer = (char *)malloc(len + 1);
			char *_buffer = buffer;
			sceIoRead(fd, buffer, len);
			buffer[len] = 0;
			sceIoClose(fd);
			for (int i = 0; i < len; i += 10) {
				buffer[9] = 0;
				favorites.push_back(buffer);
				buffer += 10;
			}
			free(_buffer);
		}
	}
}

void insert_favorites(char *tid) {
	favorites.push_back(tid);
	SceUID fd = sceIoOpen("ux0:data/VitaDB/favorites.txt", SCE_O_WRONLY | SCE_O_CREAT, 0777);
	uint64_t len = sceIoLseek(fd, 0, SCE_SEEK_END);
	char buffer[12];
	if (len > 0) {
		sprintf(buffer, ";%04d", atoi(tid));
		sceIoWrite(fd, buffer, 5);
	} else {
		sprintf(buffer, "%04d", atoi(tid));
		sceIoWrite(fd, buffer, 4);
	}
	sceIoClose(fd);
}

void remove_favorites(char *tid) {
	if (favorites.size() > 1) {
		char padded_id[12];
		sprintf(padded_id, ";%04d", atoi(tid));
		char *buffer = (char *)malloc(favorites.size() * 5 + 1);
		buffer[0] = '.';
		buffer[1] = 0;
		int idx = 0;
		int to_delete = 0;
		for (std::string& s : favorites) {
			if (s == padded_id) {
				to_delete = idx;
			} else {
				strcat(buffer, s.c_str());
				strcat(buffer, ";");
			}
			idx++;
		}
		favorites.erase(favorites.begin() + to_delete);
		SceUID fd = sceIoOpen("ux0:data/VitaDB/favorites.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
		sceIoWrite(fd, buffer, favorites.size() * 5);
		sceIoClose(fd);
		free(buffer);
	} else {
		favorites.clear();
		sceIoRemove("ux0:data/VitaDB/favorites.txt");
	}
}

void sort_apps_list(AppSelection **start, int sort_idx) {
	// Checking for empty list
	if (!start) {
		return;
	}

	std::vector<AppSelection *> sorted;
	for (AppSelection *app = *start; app; app = app->next) {
		sorted.push_back(app);
		if (sort_idx == SORT_APPS_MOST_DOWNLOADED || sort_idx == SORT_APPS_LEAST_DOWNLOADED) {
			app->sort_key = strtoll(app->downloads, nullptr, 10);
		} else if (sort_idx == SORT_APPS_SMALLEST || sort_idx == SORT_APPS_LARGEST) {
			app->sort_key = strtoll(app->size, nullptr, 10) + strtoll(app->data_size, nullptr, 10);
		} else if ((sort_idx == SORT_APPS_HIGHEST_SCORE || sort_idx == SORT_APPS_LOWEST_SCORE) && std::isnan(app->score)) {
			app->score = 0.0f;
		}
	}
	std::stable_sort(sorted.begin(), sorted.end(), [sort_idx](const AppSelection *a, const AppSelection *b) {
		switch (sort_idx) {
		case SORT_APPS_NEWEST:
			return strcasecmp(a->date, b->date) > 0;
		case SORT_APPS_OLDEST:
			return strcasecmp(a->date, b->date) < 0;
		case SORT_APPS_A_Z:
			return strcasecmp(a->name, b->name) < 0;
		case SORT_APPS_Z_A:
			return strcasecmp(a->name, b->name) > 0;
		case SORT_APPS_MOST_DOWNLOADED:
		case SORT_APPS_LARGEST:
			return a->sort_key > b->sort_key;
		case SORT_APPS_LEAST_DOWNLOADED:
		case SORT_APPS_SMALLEST:
			return a->sort_key < b->sort_key;
		case SORT_APPS_HIGHEST_SCORE:
			return a->score > b->score;
		case SORT_APPS_LOWEST_SCORE:
			return a->score < b->score;
		default:
			return false;
		}
	});
	*start = sorted.front();
	for (size_t i = 1; i < sorted.size(); ++i) {
		sorted[i - 1]->next = sorted[i];
	}
	sorted.back()->next = nullptr;
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
			if (!ptr) {
				break;
			}
			ThemeSelection *node = (ThemeSelection*)malloc(sizeof(ThemeSelection));
			sprintf(fname, "ux0:data/VitaDB/previews/%s.png", name);
			if (sceIoGetstat(fname, &st) < 0) {
				missing_previews[missing_previews_num++] = node;
			}
			node->desc = nullptr;
			node->shuffle = false;
			node->search_filtered = false;
			strcpy(node->name, name);
			sprintf(fname, "ux0:data/VitaDB/themes/%s/theme.ini", node->name);
			
			if (sceIoGetstat(fname, &st) >= 0) {
				node->state = APP_UPDATED;
			} else {
				node->state = APP_UNTRACKED;
			}
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



void sort_themes_list(ThemeSelection **start, int sort_idx) {
	// Checking for empty list
	if (start == NULL) {
		return;
	}

	std::vector<ThemeSelection *> sorted;
	for (ThemeSelection *theme = *start; theme; theme = theme->next) {
		sorted.push_back(theme);
	}
	std::stable_sort(sorted.begin(), sorted.end(), [sort_idx](const ThemeSelection *a, const ThemeSelection *b) {
		int order = strcasecmp(a->name, b->name);
		return sort_idx == SORT_THEMES_A_Z ? order < 0 : order > 0;
	});
	*start = sorted.front();
	for (size_t i = 1; i < sorted.size(); ++i) {
		sorted[i - 1]->next = sorted[i];
	}
	sorted.back()->next = nullptr;
}

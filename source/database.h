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

#ifndef _DATABASE_H
#define _DATABASE_H

enum {
	APP_UNTRACKED,
	APP_OUTDATED,
	APP_UPDATED
};

enum {
	SORT_APPS_NEWEST,
	SORT_APPS_OLDEST,
	SORT_APPS_MOST_DOWNLOADED,
	SORT_APPS_LEAST_DOWNLOADED,
	SORT_APPS_A_Z,
	SORT_APPS_Z_A,
	SORT_APPS_SMALLEST,
	SORT_APPS_LARGEST
};

enum {
	SORT_THEMES_A_Z,
	SORT_THEMES_Z_A
};

enum {
	VITA_EXECUTABLE,
	PSP_EXECUTABLE,
	AUXILIARY_FILE
};

struct TrophySelection {
	char name[64];
	char *desc;
	char icon_name[32];
	char titleid[10];
	GLuint icon;
	TrophySelection *next;
};

struct AppSelection {
	char name[192];
	char icon[128];
	char author[128];
	char type[4];
	char id[8];
	char date[12];
	char titleid[10];
	char screenshots[512];
	char source_page[192];
	char release_page[192];
	char trailer[64];
	char *desc;
	char downloads[16];
	char size[16];
	char data_size[16];
	char hash[34];
	char aux_hash[34];
	char *requirements;
	char data_link[128];
	int state;
	bool trophies;
	AppSelection *next_clash;
	AppSelection *prev_clash;
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

extern const char *aux_main_files[];

extern ThemeSelection *themes;
extern AppSelection *apps;
extern AppSelection *psp_apps;
extern TrophySelection *trophies;

extern bool update_detected;

char *get_changelog(const char *file, char *id);

void populate_apps_database(const char *file, bool is_psp);
void populate_themes_database(const char *file);

void sort_apps_list(AppSelection **start, int sort_idx);
void sort_themes_list(ThemeSelection **start, int sort_idx);

#endif

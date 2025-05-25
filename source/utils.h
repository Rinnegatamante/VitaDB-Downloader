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

#ifndef _UTILS_H
#define _UTILS_H

enum{
	MODE_VITA_HBS,
	MODE_PSP_HBS,
	MODE_THEMES,
	MODES_NUM
};

extern char pspemu_dev[8];
extern int mode_idx;

float format_size(float len);
const char *format_size_str(uint64_t len);

void move_path(char *src, char *dst);
void copy_file(const char *src, const char *dst);
void recursive_rmdir(char *path);
void recursive_mkdir(char *dir);
void populate_pspemu_path();

uint64_t get_free_storage();
uint64_t get_total_storage();

char *unescape(char *src);
void calculate_md5(SceUID fd, char *hash);

void prepare_simple_drawer();
void draw_simple_texture(GLuint tex);

void prepare_bubble_drawer();
GLuint draw_bubble_icon(GLuint tex);

namespace ImGui {
void ImageRound(ImTextureID user_texture_id, const ImVec2 &size);
}

#endif

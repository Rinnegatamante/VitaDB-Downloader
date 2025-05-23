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

#ifndef _DIALOGS_H
#define _DIALOGS_H

void early_fatal_error(const char *msg);
void early_warning(const char *msg);

int init_interactive_msg_dialog(const char *msg, ...);
int init_msg_dialog(const char *msg, ...);
int init_progressbar_dialog(const char *msg, ...);
int init_interactive_ime_dialog(const char *msg, const char *start_text);
int init_warning(const char *fmt, ...);

void draw_extractor_dialog(int index, float file_extracted_bytes, float extracted_bytes, float file_total_bytes, float total_bytes, char *filename, int num_files);
void draw_dearchiver_dialog(float file_extracted_bytes, float file_total_bytes, char *filename, GLuint bg_tex = 0);
void draw_downloader_dialog(int index, float downloaded_bytes, float total_bytes, char *text, int passes, bool self_contained, GLuint bg_tex = 0);
void draw_text_dialog(char *text, bool self_contained, bool clear_screen);

void get_dialog_text_result(char *text);

extern bool is_ime_active;

#endif

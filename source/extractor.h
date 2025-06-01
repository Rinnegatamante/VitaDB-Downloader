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

#ifndef _EXTRACTOR_H
#define _EXTRACTOR_H

void init_read_buffer();
void early_extract_zip_file(char *file, char *dir);

bool extract_psarc_file(char *file, char *out_dir, bool cancelable = false, GLuint bg_tex = 0);
bool extract_zip_file(char *file, char *dir, bool indexing, bool cancelable = false);

#endif

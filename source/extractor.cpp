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
#include <vitasdk.h>
#include <imgui_vita.h>
#include <vitaGL.h>
#include <stdio.h>
#include <malloc.h>
#include "dialogs.h"
#include "extractor.h"
#include "fios.h"
#include "unzip.h"
#include "utils.h"

#define READ_BUFFER_SIZE (128 * 1024)
static char fname[512], ext_fname[512], *read_buffer;
static SceFiosBuffer psarc_buf;

extern int SCE_CTRL_CANCEL;

void init_read_buffer() {
	read_buffer = (char *)memalign(64, READ_BUFFER_SIZE);
}

void early_extract_zip_file(char *file, char *dir) {
	init_progressbar_dialog("Extracting ShaRKF00D"); // Hardcoded for now since it's the sole instance of this function
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
		if ((zip_idx + 1) < num_files)
			unzGoToNextFile(zipfile);
	}
	unzGoToFirstFile(zipfile);
	uint32_t prog_delta = 100 / num_files;
	for (int zip_idx = 0; zip_idx < num_files; ++zip_idx) {
		unzGetCurrentFileInfo(zipfile, &file_info, fname, 512, NULL, 0, NULL, 0);
		sprintf(ext_fname, "%s/%s", dir, fname);
		const size_t filename_length = strlen(ext_fname);
		if (ext_fname[filename_length - 1] != '/') {
			curr_file_bytes = 0;
			unzOpenCurrentFile(zipfile);
			recursive_mkdir(ext_fname);
			SceUID f = sceIoOpen(ext_fname, SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0777);
			while (curr_file_bytes < file_info.uncompressed_size) {
				int rbytes = unzReadCurrentFile(zipfile, read_buffer, READ_BUFFER_SIZE);
				if (rbytes > 0) {
					sceIoWrite(f, read_buffer, rbytes);
					curr_extracted_bytes += rbytes;
					curr_file_bytes += rbytes;
				}
				sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
				vglSwapBuffers(GL_TRUE);
			}
			sceIoClose(f);
			unzCloseCurrentFile(zipfile);
		}
		sceMsgDialogProgressBarInc(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, prog_delta);
		if ((zip_idx + 1) < num_files)
			unzGoToNextFile(zipfile);
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

static bool extract_psarc_dir(int dir, char *out_dir, bool cancelable, GLuint bg_tex) {
	char path[512];
	SceFiosDirEntry entry;
	int ret;
	while (sceFiosDHReadSync(NULL, dir, &entry) >= 0) {
		sprintf(path, "%s/%s", out_dir, &entry.fullPath[entry.offsetToName]);
		if (entry.statFlags & SCE_FIOS_STAT_DIRECTORY) {
			sceIoMkdir(path, 0777);
			int subdir;
			if (sceFiosDHOpenSync(NULL, &subdir, entry.fullPath, psarc_buf) < 0) {
				// Workaround for RenPy psarcs bugging out Fios dearchiver
				if (!strcmp(&entry.fullPath[entry.offsetToName], "renpy")) {
					strcat(path, "/common");
					strcat(entry.fullPath, "/common");
					sceIoMkdir(path, 0777);
					sceFiosDHOpenSync(NULL, &subdir, entry.fullPath, psarc_buf);
				}
			}
			if (!extract_psarc_dir(subdir, path, cancelable, bg_tex)) {
				sceFiosDHCloseSync(NULL, dir);
				return false;
			}
		} else {
			int f;
			sceFiosFHOpenSync(NULL, &f, entry.fullPath, NULL);
			SceUID f2 = sceIoOpen(path, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
			size_t left = entry.fileSize;
			while (left) {
				size_t read_size = MIN(READ_BUFFER_SIZE, left);
				sceFiosFHReadSync(NULL, f, read_buffer, read_size);
				sceIoWrite(f2, read_buffer, read_size);
				left -= read_size;
				draw_dearchiver_dialog(entry.fileSize - left, entry.fileSize, entry.fullPath, bg_tex);
				if (cancelable) {
					SceCtrlData pad;
					sceCtrlPeekBufferPositive(0, &pad, 1);
					if (pad.buttons & SCE_CTRL_CANCEL) {
						sceFiosFHCloseSync(NULL, f);
						sceIoClose(f2);
						sceFiosDHCloseSync(NULL, dir);						
						return false;
					}
				}
			}
			sceFiosFHCloseSync(NULL, f);
			sceIoClose(f2);
		}
	}
	sceFiosDHCloseSync(NULL, dir);
	return true;
}

bool extract_psarc_file(char *file, char *out_dir, bool cancelable, GLuint bg_tex) {
	sceClibMemset(&psarc_buf, 0, sizeof(SceFiosBuffer));
	sceFiosMountPsarc(file);
	int dir;
	sceFiosDHOpenSync(NULL, &dir, "/files", psarc_buf);
	ImGui::GetIO().MouseDrawCursor = false;
	bool ret = extract_psarc_dir(dir, out_dir, cancelable, bg_tex);
	sceFiosTerminate();
	return ret;
}

bool extract_zip_file(char *file, char *dir, bool indexing, bool cancelable) {
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
		if ((zip_idx + 1) < num_files)
			unzGoToNextFile(zipfile);
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
			SceUID f = sceIoOpen(ext_fname, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);
			while (curr_file_bytes < file_info.uncompressed_size) {
				int rbytes = unzReadCurrentFile(zipfile, read_buffer, READ_BUFFER_SIZE);
				if (rbytes > 0) {
					sceIoWrite(f, read_buffer, rbytes);
					curr_extracted_bytes += rbytes;
					curr_file_bytes += rbytes;
				}
				draw_extractor_dialog(zip_idx + 1, curr_file_bytes, curr_extracted_bytes, file_info.uncompressed_size, total_extracted_bytes, fname, num_files);
				if (cancelable) {
					SceCtrlData pad;
					sceCtrlPeekBufferPositive(0, &pad, 1);
					if (pad.buttons & SCE_CTRL_CANCEL) {
						sceIoClose(f);
						unzCloseCurrentFile(zipfile);
						unzClose(zipfile);
						return false;
					}
				}
			}
			sceIoClose(f);
			unzCloseCurrentFile(zipfile);
		}
		if ((zip_idx + 1) < num_files)
			unzGoToNextFile(zipfile);
	}
	if (indexing)
		fclose(f2);
	unzClose(zipfile);
	ImGui::GetIO().MouseDrawCursor = false;
	return true;
}
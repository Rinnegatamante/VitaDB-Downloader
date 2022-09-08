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
#include "head.h"
#include "sha1.h"

typedef struct{
	uint32_t magic;
	uint32_t version;
	uint32_t keyTableOffset;
	uint32_t dataTableOffset;
	uint32_t indexTableEntries;
} sfo_header_t;

typedef struct{
	uint16_t keyOffset;
	uint16_t param_fmt;
	uint32_t paramLen;
	uint32_t paramMaxLen;
	uint32_t dataOffset;
} sfo_entry_t;

// Taken from VHBB, thanks to devnoname120
static void fpkg_hmac(const uint8_t* data, unsigned int len, uint8_t hmac[16]) {
	SHA1_CTX ctx;
	char sha1[20];
	char buf[64];

	sha1_init(&ctx);
	sha1_update(&ctx, (BYTE*)data, len);
	sha1_final(&ctx, (BYTE*)sha1);

	sceClibMemset(buf, 0, 64);
	sceClibMemcpy(&buf[0], &sha1[4], 8);
	sceClibMemcpy(&buf[8], &sha1[4], 8);
	sceClibMemcpy(&buf[16], &sha1[12], 4);
	buf[20] = sha1[16];
	buf[21] = sha1[1];
	buf[22] = sha1[2];
	buf[23] = sha1[3];
	sceClibMemcpy(&buf[24], &buf[16], 8);

	sha1_init(&ctx);
	sha1_update(&ctx, (BYTE*)buf, 64);
	sha1_final(&ctx, (BYTE*)sha1);
	sceClibMemcpy(hmac, sha1, 16);
}
void makeHeadBin(const char *dir) {
	uint8_t hmac[16];
	uint32_t off;
	uint32_t len;
	uint32_t out;

	char head_path[256];
	char param_path[256];
	sprintf(head_path, "%s/sce_sys/package/head.bin", dir);
	sprintf(param_path, "%s/sce_sys/param.sfo", dir);
	
	SceUID fileHandle = sceIoOpen(head_path, SCE_O_RDONLY, 0777);
	if (fileHandle >= 0) {
		sceIoClose(fileHandle);
		return;
	}

	FILE* f = fopen(param_path,"rb");
	
	if (f == NULL)
		return;
	
	sfo_header_t hdr;
	fread(&hdr, sizeof(sfo_header_t), 1, f);
	
	if (hdr.magic != 0x46535000){
		fclose(f);
		return;
	}
	
	uint8_t* idx_table = (uint8_t*)malloc((sizeof(sfo_entry_t)*hdr.indexTableEntries));
	fread(idx_table, sizeof(sfo_entry_t)*hdr.indexTableEntries, 1, f);
	sfo_entry_t* entry_table = (sfo_entry_t*)idx_table;
	fseek(f, hdr.keyTableOffset, SEEK_SET);
	uint8_t* key_table = (uint8_t*)malloc(hdr.dataTableOffset - hdr.keyTableOffset);
	fread(key_table, hdr.dataTableOffset - hdr.keyTableOffset, 1, f);
	
	char titleid[12];
	char contentid[48];
	
	for (int i=0; i < hdr.indexTableEntries; i++) {
		char param_name[256];
		sprintf(param_name, "%s", (char*)&key_table[entry_table[i].keyOffset]);
			
		if (strcmp(param_name, "TITLE_ID") == 0){ // Application Title ID
			fseek(f, hdr.dataTableOffset + entry_table[i].dataOffset, SEEK_SET);
			fread(titleid, entry_table[i].paramLen, 1, f);
		} else if (strcmp(param_name, "CONTENT_ID") == 0) { // Application Content ID
			fseek(f, hdr.dataTableOffset + entry_table[i].dataOffset, SEEK_SET);
			fread(contentid, entry_table[i].paramLen, 1, f);
		}
	}

	// Free sfo buffer
	free(idx_table);
	free(key_table);

	// Allocate head.bin buffer
	uint8_t* head_bin = (uint8_t*)malloc(size_head);
	sceClibMemcpy(head_bin, head, size_head);

	// Write full title id
	char full_title_id[48];
	snprintf(full_title_id, sizeof(full_title_id), "EP9000-%s_00-0000000000000000", titleid);
	strncpy((char*)&head_bin[0x30], strlen(contentid) > 0 ? contentid : full_title_id, 48);

	// hmac of pkg header
	len = __builtin_bswap32(*(uint32_t*)&head_bin[0xD0]);
	fpkg_hmac(&head_bin[0], len, hmac);
	sceClibMemcpy(&head_bin[len], hmac, 16);

	// hmac of pkg info
	off = __builtin_bswap32(*(uint32_t*)&head_bin[0x8]);
	len = __builtin_bswap32(*(uint32_t*)&head_bin[0x10]);
	out = __builtin_bswap32(*(uint32_t*)&head_bin[0xD4]);
	fpkg_hmac(&head_bin[off], len - 64, hmac);
	sceClibMemcpy(&head_bin[out], hmac, 16);

	// hmac of everything
	len = __builtin_bswap32(*(uint32_t*)&head_bin[0xE8]);
	fpkg_hmac(&head_bin[0], len, hmac);
	sceClibMemcpy(&head_bin[len], hmac, 16);

	// Make dir
	char pkg_dir[256];
	sprintf(pkg_dir, "%s/sce_sys/package", dir);
	sceIoMkdir(pkg_dir, 0777);

	// Write head.bin
	fclose(f);
	f = fopen(head_path, "wb");
	fwrite(head_bin, 1, size_head, f);
	fclose(f);

	free(head_bin);
}

void scePromoterUtilInit() {
	uint32_t ptr[0x100] = { 0 };
	ptr[0] = 0;
	ptr[1] = (uint32_t)&ptr[0];
	uint32_t scepaf_argp[] = { 0x400000, 0xEA60, 0x40000, 0, 0 };
	sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(scepaf_argp), scepaf_argp, (SceSysmoduleOpt *)ptr);
	sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
	scePromoterUtilityInit();
}

void scePromoterUtilTerm() {
	scePromoterUtilityExit();
	sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
	SceSysmoduleOpt opt;
	sceClibMemset(&opt.flags, 0, sizeof(opt));
	sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, &opt);
}

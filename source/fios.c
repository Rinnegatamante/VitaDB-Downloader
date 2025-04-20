/* fios.c -- use FIOS2 for optimized I/O
 *
 * Copyright (C) 2021 Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#include "fios.h"

#define MAX_PATH_LENGTH 256
#define RAMCACHEBLOCKSIZE (128 * 1024)
#define PSARCCACHEBLOCKSIZE (192 * 1024)
#define RAMCACHEBLOCKNUM 512

static int64_t g_OpStorage[SCE_FIOS_OP_STORAGE_SIZE(64, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_ChunkStorage[SCE_FIOS_CHUNK_STORAGE_SIZE(1024) / sizeof(int64_t) + 1];
static int64_t g_FHStorage[SCE_FIOS_FH_STORAGE_SIZE(1024, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_DHStorage[SCE_FIOS_DH_STORAGE_SIZE(32, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];

static SceFiosPsarcDearchiverContext g_PsarcContext;
static int32_t g_PsarcHandle;
static SceFiosBuffer g_MountBuffer;

int sceFiosMountPsarc(const char *fname) {
	int res;

	SceFiosParams params = SCE_FIOS_PARAMS_INITIALIZER;
	params.opStorage.pPtr = g_OpStorage;
	params.opStorage.length = sizeof(g_OpStorage);
	params.chunkStorage.pPtr = g_ChunkStorage;
	params.chunkStorage.length = sizeof(g_ChunkStorage);
	params.fhStorage.pPtr = g_FHStorage;
	params.fhStorage.length = sizeof(g_FHStorage);
	params.dhStorage.pPtr = g_DHStorage;
	params.dhStorage.length = sizeof(g_DHStorage);
	params.pathMax = MAX_PATH_LENGTH;

	params.threadAffinity[SCE_FIOS_IO_THREAD] = 0x20000;
	params.threadAffinity[SCE_FIOS_CALLBACK_THREAD] = 0;
	params.threadAffinity[SCE_FIOS_DECOMPRESSOR_THREAD] = 0;

	params.threadPriority[SCE_FIOS_IO_THREAD] = 64;
	params.threadPriority[SCE_FIOS_CALLBACK_THREAD] = 191;
	params.threadPriority[SCE_FIOS_DECOMPRESSOR_THREAD] = 191;

	res = sceFiosInitialize(&params);
	if (res < 0)
		return res;
	
	sceClibMemset(&g_PsarcContext, 0, sizeof(SceFiosPsarcDearchiverContext));
	g_PsarcContext.size = sizeof(SceFiosPsarcDearchiverContext);
	g_PsarcContext.pWorkBuffer = memalign(64, PSARCCACHEBLOCKSIZE);
	g_PsarcContext.workBufferSize = PSARCCACHEBLOCKSIZE;
	res = sceFiosIOFilterAdd(0, sceFiosIOFilterPsarcDearchiver, &g_PsarcContext);
	if (res < 0)
		return res;
	
	res = sceFiosArchiveGetMountBufferSizeSync(NULL, fname, NULL);
	if (res < 0)
		return res;

	g_MountBuffer.length = res;
	g_MountBuffer.pPtr = malloc(res);
	
	res = sceFiosArchiveMountSync(NULL, &g_PsarcHandle, fname, "/", g_MountBuffer, NULL);
	if (res < 0)
		return res;

	return 0;
}

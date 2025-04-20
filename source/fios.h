#ifndef __FIOS_H__
#define __FIOS_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_FIOS_FH_SIZE 80
#define SCE_FIOS_DH_SIZE 80
#define SCE_FIOS_OP_SIZE 168
#define SCE_FIOS_CHUNK_SIZE 64

#define SCE_FIOS_STAT_DIRECTORY 1

#define SCE_FIOS_PATH_MAX (1024)

#define SCE_FIOS_ALIGN_UP(val, align) (((val) + ((align) - 1)) & ~((align) - 1))
#define SCE_FIOS_STORAGE_SIZE(num, size) (((num) * (size)) + SCE_FIOS_ALIGN_UP(SCE_FIOS_ALIGN_UP((num), 8) / 8, 8))

#define SCE_FIOS_DH_STORAGE_SIZE(numDHs, pathMax) SCE_FIOS_STORAGE_SIZE(numDHs, SCE_FIOS_DH_SIZE + pathMax)
#define SCE_FIOS_FH_STORAGE_SIZE(numFHs, pathMax) SCE_FIOS_STORAGE_SIZE(numFHs, SCE_FIOS_FH_SIZE + pathMax)
#define SCE_FIOS_OP_STORAGE_SIZE(numOps, pathMax) SCE_FIOS_STORAGE_SIZE(numOps, SCE_FIOS_OP_SIZE + pathMax)
#define SCE_FIOS_CHUNK_STORAGE_SIZE(numChunks) SCE_FIOS_STORAGE_SIZE(numChunks, SCE_FIOS_CHUNK_SIZE)

#define SCE_FIOS_BUFFER_INITIALIZER	{ 0, 0 }
#define SCE_FIOS_PARAMS_INITIALIZER { 0, sizeof(SceFiosParams), 0, 0, 2, 1, 0, 0, 256 * 1024, 2, 0, 0, 0, 0, 0, SCE_FIOS_BUFFER_INITIALIZER, SCE_FIOS_BUFFER_INITIALIZER, SCE_FIOS_BUFFER_INITIALIZER, SCE_FIOS_BUFFER_INITIALIZER, NULL, NULL, NULL, { 66, 189, 66 }, { 0x40000, 0, 0x40000}, { 8 * 1024, 16 * 1024, 8 * 1024}}
#define SCE_FIOS_RAM_CACHE_CONTEXT_INITIALIZER { sizeof(SceFiosRamCacheContext), 0, (64 * 1024), NULL, NULL, 0, {0, 0, 0} }

typedef int (*SceFiosOpCallback)(void *ctx, int32_t op, uint8_t event, int err);

typedef struct SceFiosPsarcDearchiverContext
{
	size_t size;
	size_t  workBufferSize;
	void *pWorkBuffer;
	intptr_t flags;
	intptr_t reserved[3];
} SceFiosPsarcDearchiverContext;

typedef struct SceFiosOpAttr {
    int64_t deadline;
    SceFiosOpCallback callback;
    void *callbackCtx;
    int32_t priority : 8;
    uint32_t opflags : 24;
    uint32_t userTag;
    void *userPtr;
    void *reserved;
} SceFiosOpAttr;

typedef enum SceFiosOpenFlags {
    SCE_FIOS_O_RDONLY = (1U<<0),
    SCE_FIOS_O_WRONLY = (1U<<1),
    SCE_FIOS_O_RDWR = (SCE_FIOS_O_RDONLY | SCE_FIOS_O_WRONLY),
    SCE_FIOS_O_APPEND = (1U<<2),
    SCE_FIOS_O_CREAT = (1U<<3),
    SCE_FIOS_O_TRUNC = (1U<<4),
} SceFiosOpenFlags;



typedef enum SceFiosThreadType {
	SCE_FIOS_IO_THREAD = 0,
	SCE_FIOS_DECOMPRESSOR_THREAD = 1,
	SCE_FIOS_CALLBACK_THREAD = 2,
	SCE_FIOS_THREAD_TYPES = 3
} SceFiosThreadType;

typedef struct SceFiosRamCacheContext {
	size_t sizeOfContext;
	size_t workBufferSize;
	size_t blockSize;
	void *pWorkBuffer;
	const char *pPath;
	intptr_t flags;
	intptr_t reserved[3];
} SceFiosRamCacheContext;

typedef struct SceFiosBuffer {
	void *pPtr;
	size_t length;
} SceFiosBuffer;

typedef struct SceFiosDirEntry {
    int64_t fileSize;
    uint32_t statFlags;
    uint16_t nameLength;
    uint16_t fullPathLength;
    uint16_t offsetToName;
    uint16_t reserved[3];
    char fullPath[SCE_FIOS_PATH_MAX];
} SceFiosDirEntry;

typedef struct SceFiosOpenParams {
    uint32_t openFlags : 16;
    uint32_t opFlags : 16;
    uint32_t reserved;
    SceFiosBuffer buffer;
} SceFiosOpenParams;

typedef struct SceFiosParams {
	uint32_t initialized : 1;
	uint32_t paramsSize : 15;
	uint32_t pathMax : 16;
	uint32_t profiling;
	uint32_t ioThreadCount;
	uint32_t threadsPerScheduler;
	uint32_t extraFlag1 : 1;
	uint32_t extraFlags : 31;
	uint32_t maxChunk;
	uint8_t maxDecompressorThreadCount;
	uint8_t reserved1;
	uint8_t reserved2;
	uint8_t reserved3;
	intptr_t reserved4;
	intptr_t reserved5;
	SceFiosBuffer opStorage;
	SceFiosBuffer fhStorage;
	SceFiosBuffer dhStorage;
	SceFiosBuffer chunkStorage;
	void *pVprintf;
	void *pMemcpy;
	void *pProfileCallback;
	int threadPriority[3];
	int threadAffinity[3];
	int threadStackSize[3];
} SceFiosParams;

int sceFiosInitialize(const SceFiosParams *params);
void sceFiosTerminate();

int sceFiosIOFilterAdd(int index, void *pFilterCallback, void *pFilterContext);
void sceFiosIOFilterCache();
void sceFiosIOFilterPsarcDearchiver();
int64_t sceFiosFHReadSync(SceFiosOpAttr *attr, int32_t fh, void *pBuf, int64_t length);
int64_t sceFiosFHSeek(int32_t fh, int64_t offset, int32_t whence);

int64_t sceFiosArchiveGetMountBufferSizeSync(SceFiosOpAttr *attr, const char *path, SceFiosOpenParams *params);
int sceFiosArchiveMountSync(SceFiosOpAttr *attr, int *handle, const char *path, const char *mountpoint, SceFiosBuffer mountBuffer, SceFiosOpenParams *params);

int sceFiosFHCloseSync(SceFiosOpAttr *attr, int handle);
int sceFiosFHClose(SceFiosOpAttr *attr, int handle);
int sceFiosDHCloseSync(SceFiosOpAttr *attr, int handle);
int sceFiosDHClose(SceFiosOpAttr *attr, int handle);
int sceFiosFHOpenSync(SceFiosOpAttr *attr, int *handle, const char *path, SceFiosOpenParams *params);
int sceFiosFHOpen(SceFiosOpAttr *attr, int *handle, const char *path, SceFiosOpenParams *params);
int sceFiosDHOpenSync(SceFiosOpAttr *attr, int *handle, const char *path, SceFiosBuffer buf);
int sceFiosDHReadSync(SceFiosOpAttr *attr, int handle, SceFiosDirEntry *entry);
int64_t sceFiosFHReadSync(SceFiosOpAttr *attr, int handle, void *buf, int64_t len);

int sceFiosFHPread(SceFiosOpAttr *pAttr, int handle, void *buf, int64_t length, int64_t offset);
int sceFiosFHPwrite(SceFiosOpAttr *pAttr, int handle, void *buf, int64_t length, int64_t offset);
int sceFiosOpWait(int op);

int sceFiosMountPsarc(const char *fname);

uint8_t sceFiosIsIdle();

#ifdef __cplusplus
};
#endif

#endif

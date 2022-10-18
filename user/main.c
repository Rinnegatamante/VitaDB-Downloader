#include <vitasdk.h>
#include <taihen.h>
#include <string.h>
#include <libk/stdlib.h>
#include <libk/stdio.h>

static SceUID sceClibPrintf_hook, sceIoRemove_hook;
static tai_hook_ref_t sceClibPrintf_ref, sceIoRemove_ref;

int sceClibPrintf_patched(const char *txt, ...) {
	if (strncmp(txt, "[%4s/%6s: %-26s] ***********************************************", 64) == 0) {
		sceAppMgrLaunchAppByName(0x60000, "VITADBDLD", "");
	}
	return 0;
}

int sceIoRemove_patched(const char *name) {
	if (strncmp(name, "ux0:/ShaRKF00D/libshacccg.suprx", 31) == 0) {
		TAI_CONTINUE(int, sceIoRemove_ref, name);
		sceAppMgrLaunchAppByName(0x60000, "VITADBDLD", "");
	}
	return 0;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	sceClibPrintf_hook = taiHookFunctionImport(&sceClibPrintf_ref, TAI_MAIN_MODULE, 0xCAE9ACE6, 0xFA26BC62, sceClibPrintf_patched);
	sceIoRemove_hook = taiHookFunctionImport(&sceIoRemove_ref, TAI_MAIN_MODULE, 0xCAE9ACE6, 0xE20ED0F3, sceIoRemove_patched);

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	taiHookRelease(sceClibPrintf_hook, sceClibPrintf_ref);
	taiHookRelease(sceIoRemove_hook, sceIoRemove_ref);
	return SCE_KERNEL_STOP_SUCCESS;
}

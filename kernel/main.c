#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <vitasdkkern.h>
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/fios2.h>
#include <taihen.h>

static int sceSblQafMgrIsAllowLimitedDebugMenuDisplay_hook = -1;
static tai_hook_ref_t sceSblQafMgrIsAllowLimitedDebugMenuDisplay_ref;

static SceFiosOverlay fios_overlay;
static int res;

// Taken from CrystalPSM (https://github.com/EliCrystal2001/CrystalPSM/blob/main/CrystalDriver/CrystalDriver.c)
int create_cb(SceUID Pid, SceProcEventInvokeParam2 *Param, int Unknown) {
	memset(&fios_overlay, 0x00, sizeof(SceFiosOverlay));
	
	fios_overlay.type = SCE_FIOS_OVERLAY_TYPE_OPAQUE;
	fios_overlay.order = 0xFF;
	fios_overlay.dst_len = strlen("host0:/package") + 1;
	fios_overlay.src_len = strlen("ux0:/data") + 1;
	fios_overlay.pid = Pid;
	strncpy(fios_overlay.dst, "host0:/package", strlen("host0:/package") + 1);
	strncpy(fios_overlay.src,"ux0:/data", strlen("ux0:/data") + 1);
	res = 0;
	
	ksceFiosKernelOverlayAddForProcess(Pid, &fios_overlay, &res);
	return 0;
}

int sceSblQafMgrIsAllowLimitedDebugMenuDisplay_patched() {
	if (sceSblQafMgrIsAllowLimitedDebugMenuDisplay_hook >= 0)
		taiHookReleaseForKernel(sceSblQafMgrIsAllowLimitedDebugMenuDisplay_hook, sceSblQafMgrIsAllowLimitedDebugMenuDisplay_ref);
	return 1;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args) {
	taiReloadConfigForKernel(0, 0);
	
	SceProcEventHandler event_handler;
	memset(&event_handler, 0, sizeof(SceProcEventHandler));
	event_handler.size = sizeof(SceProcEventHandler);
	event_handler.create = create_cb;

	ksceKernelRegisterProcEventHandler("Host2Ux0", &event_handler, 0);
	sceSblQafMgrIsAllowLimitedDebugMenuDisplay_ref = taiHookFunctionExportForKernel(KERNEL_PID, &sceSblQafMgrIsAllowLimitedDebugMenuDisplay_ref, "SceSblSsMgr", 0x756B7E89, 0xC456212D, sceSblQafMgrIsAllowLimitedDebugMenuDisplay_patched);	
	
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
	if (sceSblQafMgrIsAllowLimitedDebugMenuDisplay_hook >= 0)
		taiHookReleaseForKernel(sceSblQafMgrIsAllowLimitedDebugMenuDisplay_hook, sceSblQafMgrIsAllowLimitedDebugMenuDisplay_ref);
    return SCE_KERNEL_STOP_SUCCESS;
}

#include <string.h>
#include <vitasdkkern.h>
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/fios2.h>
#include <taihen.h>

static SceUID qaf_hook = -1;
static tai_hook_ref_t qaf_ref;
static SceFiosOverlay overlay;
static int overlay_res;

static int on_create(SceUID pid, SceProcEventInvokeParam2* param, int unk)
{
    memset(&overlay, 0, sizeof(overlay));
    overlay.type = SCE_FIOS_OVERLAY_TYPE_OPAQUE;
    overlay.order = 0xFF;
    overlay.dst_len = strlen("host0:/package") + 1;
    overlay.src_len = strlen("ux0:/data") + 1;
    overlay.pid = pid;
    strncpy(overlay.dst, "host0:/package", sizeof(overlay.dst));
    strncpy(overlay.src, "ux0:/data", sizeof(overlay.src));
    overlay_res = 0;
    ksceFiosKernelOverlayAddForProcess(pid, &overlay, &overlay_res);
    return 0;
}

static int qaf_patched(void)
{
    if (qaf_hook >= 0) {
        taiHookReleaseForKernel(qaf_hook, qaf_ref);
        qaf_hook = -1;
    }
    return 1;
}

int module_start(SceSize argc, const void* args)
{
    taiReloadConfigForKernel(0, 0);
    SceProcEventHandler h;
    memset(&h, 0, sizeof(h));
    h.size = sizeof(h);
    h.create = on_create;
    ksceKernelRegisterProcEventHandler("VpkgjPsm", &h, 0);
    qaf_hook = taiHookFunctionExportForKernel(KERNEL_PID, &qaf_ref, "SceSblSsMgr", 0x756B7E89, 0xC456212D, qaf_patched);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void* args)
{
    if (qaf_hook >= 0)
        taiHookReleaseForKernel(qaf_hook, qaf_ref);
    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize argc, const void* args) __attribute__((weak, alias("module_start")));

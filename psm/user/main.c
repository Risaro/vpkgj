#include <vitasdk.h>
#include <taihen.h>
#include <string.h>

static SceUID printf_hook = -1, remove_hook = -1;
static tai_hook_ref_t printf_ref, remove_ref;

static int printf_patched(const char* txt, ...)
{
    if (strncmp(txt, "[%4s/%6s: %-26s] ***********************************************", 64) == 0)
        sceAppMgrLaunchAppByName(0x60000, "VPKG00001", "");
    return 0;
}

static int remove_patched(const char* name)
{
    int r = TAI_CONTINUE(int, remove_ref, name);
    if (strncmp(name, "ux0:/ShaRKF00D/libshacccg.suprx", 31) == 0)
        sceAppMgrLaunchAppByName(0x60000, "VPKG00001", "");
    return r;
}

int module_start(SceSize argc, const void* args)
{
    printf_hook = taiHookFunctionImport(&printf_ref, TAI_MAIN_MODULE, 0xCAE9ACE6, 0xFA26BC62, printf_patched);
    remove_hook = taiHookFunctionImport(&remove_ref, TAI_MAIN_MODULE, 0xCAE9ACE6, 0xE20ED0F3, remove_patched);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void* args)
{
    if (printf_hook >= 0)
        taiHookRelease(printf_hook, printf_ref);
    if (remove_hook >= 0)
        taiHookRelease(remove_hook, remove_ref);
    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize argc, const void* args) __attribute__((weak, alias("module_start")));

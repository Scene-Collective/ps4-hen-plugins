extern "C"
{
#include "../../common/entry.h"

#include <stdint.h>
#include <stdio.h>
#include "../../common/notify.h"
#include "../../common/plugin_common.h"
#include "../../common/function_ptr.h"
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include <pthread.h>
#include <unistd.h>

#include "memory.h"
#include "mono.h"

#include "patches.h"

#include "hen_settings_icon.inc.c"
#include "hen_settings.inc.c"
#include "../../common/file.h"

attr_public const char* g_pluginName = "PhantomHEN ShellUI";
attr_public const char* g_pluginDesc = "PhantomHEN settings menu, theming and ShellUI patches. Fork of Scene-Collective/ps4-hen-plugins.";
attr_public const char* g_pluginAuth = "iDntUseModz (upstream: illusiony)";
attr_public const char* g_pluginVersion = "Git Commit: " GIT_COMMIT
                                          "\n"
                                          "Git Branch: " GIT_VER
                                          "\n"
                                          "Git Commit Number: " GIT_NUM_STR
                                          "\n"
                                          "Built: " BUILD_DATE;

static void open_console(void)
{
    extern int open(const char*, int);
    extern int close(int);
    const int O_WRONLY = 2;
    const int stdout_ = 1;
    const int stderr_ = 2;
    const int fd = open("/dev/console", O_WRONLY);
    if (fd > 0)
    {
        dup2(fd, stdout_);
        dup2(fd, stderr_);
        close(fd);
    }
}

static void MainThreadCheck(void)
{
    const char* sandbox_path = sceKernelGetFsSandboxRandomWord();
    if (sandbox_path)
    {
        char core_dll[260] = {};
        snprintf(core_dll, sizeof(core_dll), "/%s/common/lib/Sce.PlayStation.Core.dll", sandbox_path);
        void* ptr = mono_get_image(core_dll);
        if (ptr)
        {
            void* check = Mono_Get_Address_of_Method(ptr, "Sce.PlayStation.Core.Runtime", "Diagnostics", "CheckRunningOnMainThread", 0);
            if (check)
            {
                uint8_t ret = 0xc3;
                sys_proc_rw(getpid(), (uintptr_t)check, &ret, sizeof(ret), 1);
            }
        }
    }
}

void PrintTimeTick(void)
{
    static bool once = false;
    if (!once)
    {
        {
            void* app_exe = mono_get_image("/app0/psm/Application/app.exe");
            if (app_exe)
            {
                App_Exe = app_exe;
                void (*createDevKitPanel)(void*) = (void (*)(void*))Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.TopMenu", "AreaManager", "createDevKitPanel", 0);
                printf("createDevKitPanel 0x%p\n", createDevKitPanel);
                if (createDevKitPanel)
                {
                    createDevKitPanel(Mono_Get_InstanceEx(App_Exe, "Sce.Vsh.ShellUI.TopMenu", "AreaManager", "Instance"));
                }
            }
            UploadNewCorelibStreamReader();
            MainThreadCheck();
        }
        once = true;
    }
}

static int sceKernelGetHwSerialNumber_hook(char* buf)
{
    if (buf)
    {
        const int sz = 18;
        memset(buf, 0, sz);
        memset(buf, '0', sz - 1);
    }
    return 0;
}

static void PatchDebugSettings(void)
{
    uintptr_t sceKernelGetUtokenStoreModeForRcmgr = 0;
    uintptr_t sceKernelGetDebugMenuModeForRcmgr = 0;
    sceKernelDlsym(0x2001, "sceKernelGetUtokenStoreModeForRcmgr", (void**)&sceKernelGetUtokenStoreModeForRcmgr);
    sceKernelDlsym(0x2001, "sceKernelGetDebugMenuModeForRcmgr", (void**)&sceKernelGetDebugMenuModeForRcmgr);
    static const uint8_t m[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    const int pid = getpid();
    if (sceKernelGetUtokenStoreModeForRcmgr)
    {
        sys_proc_rw(pid, sceKernelGetUtokenStoreModeForRcmgr, m, sizeof(m), 1);
    }
    if (sceKernelGetDebugMenuModeForRcmgr)
    {
        sys_proc_rw(pid, sceKernelGetDebugMenuModeForRcmgr, m, sizeof(m), 1);
    }
}

static void RunPost(const int app_exe)
{
    struct OrbisKernelModuleInfo info = {0};
    info.size = sizeof(info);
    const int r = sceKernelGetModuleInfo(0, &info);
    if (r == 0)
    {
        UploadEventProxyCall(&info);
        if (app_exe > 0)
        {
            struct OrbisKernelModuleInfo info_app = {0};
            info_app.size = sizeof(info_app);
            const int r = sceKernelGetModuleInfo(app_exe, &info_app);
            if (r == 0)
            {
                UploadRegStrCall(&info, &info_app);
                UploadRemotePlayPatch(&info_app);
                UploadShellUICheck();
                UploadBgftFixup(&info);
                UploadFinishBootEffectCode(&info_app);
            }
        }
    }
    uintptr_t sceKernelGetHwSerialNumber = 0;
    sceKernelDlsym(0x2001, "sceKernelGetHwSerialNumber", (void**)&sceKernelGetHwSerialNumber);
    if (sceKernelGetHwSerialNumber)
    {
        WriteJump64(sceKernelGetHwSerialNumber, (uintptr_t)sceKernelGetHwSerialNumber_hook);
    }
    UploadDebugSettingsPatch();
}

uiTYPEDEF_FUNCTION_PTR(int, sceKernelLoadStartModuleInternalForMono_original, const char* param_1, void* param_2, void* param_3, void* param_4, void* param_5, void* param_6);

static int sceKernelLoadStartModuleInternalForMono_Hook(const char* param_1, void* param_2, void* param_3, void* param_4, void* param_5, void* param_6)
{
    const int md = sceKernelLoadStartModuleInternalForMono_original.ptr(param_1, param_2, param_3, param_4, param_5, param_6);
    debug_printf("path %s load returned 0x%08x\n", param_1, md);
    static bool test = false;
    static int app_exe_h = 0;
    if (!test && md > 0 && strstr(param_1, "/app0/psm/Application/app.exe.sprx"))
    {
        app_exe_h = md;
        RunPost(md);
        test = true;
    }
    else if (test)
    {
        if (app_exe_h > 0)
        {
            static bool once = false;
            if (!Root_Domain)
            {
                Root_Domain = mono_get_root_domain();
            }
            else if (!once && Root_Domain && !App_Exe)
            {
                App_Exe = mono_get_image("/app0/psm/Application/app.exe");
                if (App_Exe)
                {
                    ffinal_printf("app_exe: 0x%p\n", App_Exe);
                    UploadNewPkgInstallerPath(App_Exe, app_exe_h);
                    UploadOnBranch(App_Exe);
                    once = true;
                }
            }
        }
    }
    return md;
}

static void UploadMonoCall(void)
{
    uintptr_t monoCall = 0;
    sceKernelDlsym(0x2001, "sceKernelLoadStartModuleInternalForMono", (void**)&monoCall);
    if (monoCall)
    {
        const uint8_t* monoByte = (uint8_t*)monoCall;
        if (monoByte[0] == 0xe9)
        {
            sceKernelLoadStartModuleInternalForMono_original.addr = ReadLEA32(monoCall, 0, 1, 5);
            WriteJump64(monoCall, (uintptr_t)sceKernelLoadStartModuleInternalForMono_Hook);
        }
    }
}

attr_public int plugin_load(struct SceEntry* args)
{
    open_console();
    mkdir(BASE_PATH, 0777);
    mkdir(SHELLUI_DATA_PATH, 0777);
    mkdir(SHELLUI_ICONS_PATH, 0777);
    mkdir(USER_PLUGIN_PATH, 0777);
    write_file(SHELLUI_HEN_SETTINGS, data_hen_settings_xml, data_hen_settings_xml_len);
    write_file(SHELLUI_HEN_SETTINGS_ICON_PATH, data_hen_settings_icon_png, data_hen_settings_icon_png_len);
    printf("====\n\nHello from mono module\n\n====\n");
    if (0 && file_exists_temp(g_pluginName) == 0)
    {
        Notify("", "Attempted to load %s again! Did it crash previously?\n", g_pluginName);
        return 0;
    }
    touch_temp(g_pluginName);
    puts(g_pluginVersion);
    UploadMonoCall();
    return 0;
}
}

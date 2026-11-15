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
#include "../../common/ini.h"
#include "../../common/path.h"

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

// Check if a flag file exists (created by settings_menu.c btn_jb_apply)
static int flag_enabled(const char* name)
{
    char path[128];
    snprintf(path, sizeof(path), BASE_PATH "/flags/%s", name);
    char buf[4] = {0};
    if (read_file(path, buf, sizeof(buf) - 1) == 0 && buf[0] == '1')
        return 1;
    return 0;
}

// ini_set_default: write key only if it doesn't already have a value.
static void ini_set_default(INIFile* ini, const char* section, const char* key, const char* def)
{
    if (!ini_get(ini, section, key))
        ini_set(ini, section, key, def);
}

// SeedPersistentINI — called once from plugin_load.
// Creates CONFIG_INI_PATH with all persistent keys at their defaults.
// Without this, NewOnPreCreate gets an ini_load failure on first boot
// and toggles never round-trip, so nothing ever persists.
static void SeedPersistentINI(void)
{
    // Touch the file first so ini_load can open it
    int fd = open(CONFIG_INI_PATH, 2 | 0x200 | 0x800, 0644); // O_WRONLY|O_CREAT|O_TRUNC
    if (fd >= 0) close(fd);

    INIFile* ini = ini_load(CONFIG_INI_PATH);
    if (!ini) return;

    // ── jailbreak_mods defaults ────────────────────────────────────────────
    ini_set_default(ini, "jailbreak_mods", "jb_debug_menu",   "0");
    ini_set_default(ini, "jailbreak_mods", "jb_dipsw",        "0");
    ini_set_default(ini, "jailbreak_mods", "jb_qa_flag",      "0");
    ini_set_default(ini, "jailbreak_mods", "jb_testkit",      "0");
    ini_set_default(ini, "jailbreak_mods", "jb_devkit",       "0");
    ini_set_default(ini, "jailbreak_mods", "jb_lan_bypass",   "0");
    ini_set_default(ini, "jailbreak_mods", "jb_dns_block",    "0");
    ini_set_default(ini, "jailbreak_mods", "jb_p2p",          "0");
    ini_set_default(ini, "jailbreak_mods", "jb_region_free",  "0");
    ini_set_default(ini, "jailbreak_mods", "jb_trophy_lock",  "0");
    ini_set_default(ini, "jailbreak_mods", "jb_patch_skip",   "0");
    ini_set_default(ini, "jailbreak_mods", "jb_disc_debug",   "0");
    ini_set_default(ini, "jailbreak_mods", "jb_no_sleep",     "0");
    ini_set_default(ini, "jailbreak_mods", "jb_fan_max",      "0");
    ini_set_default(ini, "jailbreak_mods", "jb_fan_silent",   "0");
    ini_set_default(ini, "jailbreak_mods", "jb_boost",        "0");
    ini_set_default(ini, "jailbreak_mods", "jb_bc2",          "0");
    ini_set_default(ini, "jailbreak_mods", "jb_save_resign",  "0");
    ini_set_default(ini, "jailbreak_mods", "jb_trophy_sync",  "0");

    // ── ui_tweaks defaults ─────────────────────────────────────────────────
    ini_set_default(ini, "ui_tweaks", "inp_font_size",   "24");
    ini_set_default(ini, "ui_tweaks", "inp_text_color",  "FFFFFFFF");
    ini_set_default(ini, "ui_tweaks", "inp_username",    "");
    ini_set_default(ini, "ui_tweaks", "ui_hide_clock",   "0");
    ini_set_default(ini, "ui_tweaks", "ui_hide_notif",   "0");
    ini_set_default(ini, "ui_tweaks", "ui_dark_icons",   "0");
    ini_set_default(ini, "ui_tweaks", "ui_grid_compact", "0");
    ini_set_default(ini, "ui_tweaks", "ui_perf_overlay", "0");
    ini_set_default(ini, "ui_tweaks", "ui_esp_overlay",  "0");

    // ── psn_bypass defaults — default ON (that's why they installed HEN) ──
    ini_set_default(ini, "psn_bypass", "psn_plus_bypass",   "1");
    ini_set_default(ini, "psn_bypass", "psn_plus_ui",       "1");
    ini_set_default(ini, "psn_bypass", "psn_signin_bypass", "1");
    ini_set_default(ini, "psn_bypass", "psn_offline_mode",  "0");
    ini_set_default(ini, "psn_bypass", "psn_spoof_idps",    "0");
    ini_set_default(ini, "psn_bypass", "psn_block_reports", "0");

    ini_save(ini);
    ini_free(ini);
}

// ApplyPersistentPatches — called from RunPost (after dlsym is live).
// Re-applies every kernel-level patch the user enabled so settings
// survive across reboots — without this nothing sticks.
static void ApplyPersistentPatches(void)
{
    INIFile* ini = ini_load(CONFIG_INI_PATH);
    if (!ini) return;

    // pini_on: read a key from ini; returns true if value is "1"
    // Written as a lambda so it closes over `ini` without GNU statement exprs.
    auto pini_on = [&](const char* sec, const char* key) -> bool {
        const char* _v = ini_get(ini, sec, key);
        return (_v && _v[0] == '1');
    };

    // ── PS Plus bypass ─────────────────────────────────────────────────────
    // Write flag file; plugin_server / kpayload reads it on its next tick.
    if (pini_on("psn_bypass", "psn_plus_bypass"))
        write_file(BASE_PATH "/flags/ps_plus_bypass", "1", 1);
    else
        unlink(BASE_PATH "/flags/ps_plus_bypass");

    // ── PSN sign-in bypass ─────────────────────────────────────────────────
    if (pini_on("psn_bypass", "psn_signin_bypass"))
        write_file(BASE_PATH "/flags/psn_signin_bypass", "1", 1);
    else
        unlink(BASE_PATH "/flags/psn_signin_bypass");

    // ── Auto sleep disable ─────────────────────────────────────────────────
    if (pini_on("jailbreak_mods", "jb_no_sleep"))
    {
        typedef int (*sceRegMgrSetInt_t)(unsigned int, int);
        sceRegMgrSetInt_t fn_reg = NULL;
        sceKernelDlsym(0x2001, "sceRegMgrSetInt", (void**)&fn_reg);
        if (fn_reg) fn_reg(0x1B200000, 0);
    }

    // ── QA flag ───────────────────────────────────────────────────────────
    if (pini_on("jailbreak_mods", "jb_qa_flag"))
    {
        typedef int (*sceRegMgrSetInt_t)(unsigned int, int);
        sceRegMgrSetInt_t fn_reg = NULL;
        sceKernelDlsym(0x2001, "sceRegMgrSetInt", (void**)&fn_reg);
        if (fn_reg) fn_reg(0x3C070000, 1);
    }

    // ── DipSw override ────────────────────────────────────────────────────
    if (pini_on("jailbreak_mods", "jb_dipsw"))
    {
        typedef int (*sceRegMgrSetInt_t)(unsigned int, int);
        sceRegMgrSetInt_t fn_reg = NULL;
        sceKernelDlsym(0x2001, "sceRegMgrSetInt", (void**)&fn_reg);
        if (fn_reg)
        {
            fn_reg(0x3C040000, 0x7FFFFFFF);
            fn_reg(0x3C040001, 0x7FFFFFFF);
        }
    }

    // ── Fan control ───────────────────────────────────────────────────────
    {
        typedef int (*fan_ctl_t)(int);
        typedef int (*fan_spd_t)(int);
        fan_ctl_t fn_ctl = NULL;
        fan_spd_t fn_spd = NULL;
        sceKernelDlsym(0x2001, "sceKernelSetFanControl", (void**)&fn_ctl);
        sceKernelDlsym(0x2001, "sceKernelSetFanSpeed",   (void**)&fn_spd);
        if (fn_ctl && fn_spd)
        {
            if (pini_on("jailbreak_mods", "jb_fan_max"))
                { fn_ctl(0); fn_spd(100); }
            else if (pini_on("jailbreak_mods", "jb_fan_silent"))
                { fn_ctl(0); fn_spd(20); }
        }
    }

    // ── Boost mode ────────────────────────────────────────────────────────
    if (pini_on("jailbreak_mods", "jb_boost"))
    {
        typedef int (*boost_t)(int);
        boost_t fn_boost = NULL;
        sceKernelDlsym(0x2001, "sceKernelSetGpuCu", (void**)&fn_boost);
        if (fn_boost) fn_boost(1);
    }

    ini_free(ini);
}

static void PatchDebugSettings(void)
{
    // Only apply if the debug_menu flag is set via Jailbreak Mods settings
    if (!flag_enabled("debug_menu"))
        return;

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

// Username override — hook sceUserServiceGetUserName to return custom name
// from /data/hen/username.txt if it exists
static uintptr_t sceUserServiceGetUserName_original = 0;

static int sceUserServiceGetUserName_hook(int userId, char* name, size_t nameSize)
{
    // Try custom name first
    char custom[64] = {0};
    if (read_file(BASE_PATH "/username.txt", custom, sizeof(custom) - 1) == 0 && custom[0])
    {
        // Strip trailing newlines
        for (int i = strlen(custom) - 1; i >= 0 && (custom[i] == '\n' || custom[i] == '\r'); i--)
            custom[i] = '\0';
        if (custom[0])
        {
            strncpy(name, custom, nameSize - 1);
            name[nameSize - 1] = '\0';
            return 0;
        }
    }
    // Fall through to original
    typedef int (*fn_t)(int, char*, size_t);
    return ((fn_t)sceUserServiceGetUserName_original)(userId, name, nameSize);
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
    ApplyPersistentPatches();

    // Username override hook — only if username.txt exists
    {
        char tmp[4] = {0};
        if (read_file(BASE_PATH "/username.txt", tmp, sizeof(tmp) - 1) == 0 && tmp[0])
        {
            uintptr_t fn = 0;
            sceKernelDlsym(0x2001, "sceUserServiceGetUserName", (void**)&fn);
            if (fn)
            {
                sceUserServiceGetUserName_original = fn;
                WriteJump64(fn, (uintptr_t)sceUserServiceGetUserName_hook);
            }
        }
    }
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
    mkdir(BASE_PATH "/flags", 0777);
    mkdir(BASE_PATH "/ui", 0777);
    mkdir(SHELLUI_DATA_PATH, 0777);
    mkdir(SHELLUI_ICONS_PATH, 0777);
    mkdir(USER_PLUGIN_PATH, 0777);
    write_file(SHELLUI_HEN_SETTINGS, data_hen_settings_xml, data_hen_settings_xml_len);
    write_file(SHELLUI_HEN_SETTINGS_ICON_PATH, data_hen_settings_icon_png, data_hen_settings_icon_png_len);
    SeedPersistentINI();
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

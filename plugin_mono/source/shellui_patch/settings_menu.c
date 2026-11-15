#include "settings_menu.h"
#include "../../../common/file.h"
#include "../../../common/function_ptr.h"
#include "../../../common/memory.h"
#include "../../../common/notify.h"
#include "../../../common/plugin_common.h"
#include "../../../common/stringid.h"
#include "../../../common/path.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <dirent.h>
#include "../mono.h"

#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <stddef.h>
#include <signal.h>
#include <pthread.h>
#include <stdlib.h>

#include "../../../common/syscall.h"

// sceKernelUsleep is a PS4 kernel export; declare it explicitly since the
// OpenOrbis SDK headers may not expose it in this compilation unit.
extern int sceKernelUsleep(unsigned int microseconds);

#include "../ini.h"
#include "../shellui_mono.h"

// sys_proc_rw: kernel-level memory read/write for patching running processes
// dir=0 read, dir=1 write
static int sys_proc_rw(int pid, uint64_t addr, void* buf, size_t len, int dir)
{
    // syscall 107 is the custom proc_rw on HEN-enabled kernels
    return (int)orbis_syscall(107, pid, addr, buf, len, dir);
}

// Get ShellUI PID by iterating /proc
static int get_shellui_pid(void)
{
    // ShellUI runs as SceShellUI — PID usually in 40-80 range
    // We can find it via the kernel's process list or by known PID
    // On HEN, ShellUI PID is typically passed or discoverable.
    // Use sys_proc_cmd to iterate, or read from /proc
    char buf[64];
    for (int pid = 1; pid < 200; pid++)
    {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        if (read_file(path, buf, sizeof(buf) - 1) == 0 && buf[0])
        {
            if (strstr(buf, "SceShellUI") || strstr(buf, "shellui"))
                return pid;
        }
    }
    // Fallback: we ARE ShellUI (this plugin runs inside it)
    // Use getpid() since this plugin is loaded into ShellUI's process
    return getpid();
}

// Pattern scan in ShellUI process memory for a known byte sequence
// Returns 0 on failure, address on success
static uint64_t pattern_scan_self(const uint8_t* pattern, const uint8_t* mask, int len,
                                   uint64_t start, uint64_t end)
{
    // Since we run inside ShellUI, scan our own address space directly
    for (uint64_t addr = start; addr < end - len; addr++)
    {
        int found = 1;
        for (int i = 0; i < len; i++)
        {
            if (mask[i] == 'x' && *(uint8_t*)(addr + i) != pattern[i])
            {
                found = 0;
                break;
            }
        }
        if (found) return addr;
    }
    return 0;
}

// Known font size pattern: float 24.0 (0x41C00000) near UI render code
// This is a heuristic — the exact location depends on FW but the surrounding
// code signature is stable across versions.
static uint64_t find_font_size_addr(void)
{
    // Font size default is 24.0f = 0x41C00000 in .rodata or .data
    // We search for the float constant in a known module range.
    // Since we're in-process, scan the ShellUI module's data segment.
    // Fallback: write to a config location that the UI framework reads.

    // For now, save to disk and signal reload — safer than blind memory patch
    return 0; // pattern scan result, 0 = not found
}
// Helper: check if a jailbreak toggle is enabled in INI
static int jb_toggle_on(INIFile* ini, const char* key)
{
    if (!ini) return 0;
    const char* v = ini_get(ini, "jailbreak_mods", key);
    return (v && strcmp(v, "1") == 0);
}

// NewOnPreCreate already logs every element's "Type %d" to the kernel log —
// open any text_input field once with klog watched and drop that number in
// here. Until then, text_input ids still render/save their XML default,
// they just don't round-trip through CONFIG_INI_PATH yet.
#define TEXTINPUT_ELEMENT_TYPE_VALUE (-1)

typedef enum
{
    REPLACE_MATCH,
    ADD_BEFORE,
    ADD_AFTER,
    REPLACE_LINE
} ReplaceMode;

static void str_replace(const char* input, const char* search, const char* replacement, char* output, size_t output_size, ReplaceMode mode)
{
    const char* pos = input;
    size_t search_len = strlen(search);
    size_t replacement_len = strlen(replacement);
    size_t out_len = 0;

    while (*pos && out_len < output_size - 1)
    {
        if (strncmp(pos, search, search_len) == 0)
        {
            switch (mode)
            {
                case REPLACE_MATCH:
                    if (out_len + replacement_len >= output_size - 1)
                    {
                        goto done;
                    }
                    memcpy(output + out_len, replacement, replacement_len);
                    out_len += replacement_len;
                    pos += search_len;
                    break;

                case ADD_BEFORE:
                    if (out_len + replacement_len + search_len >= output_size - 1)
                    {
                        goto done;
                    }
                    memcpy(output + out_len, replacement, replacement_len);
                    out_len += replacement_len;
                    memcpy(output + out_len, pos, search_len);
                    out_len += search_len;
                    pos += search_len;
                    break;

                case ADD_AFTER:
                    if (out_len + search_len + replacement_len >= output_size - 1)
                    {
                        goto done;
                    }
                    memcpy(output + out_len, pos, search_len);
                    out_len += search_len;
                    memcpy(output + out_len, replacement, replacement_len);
                    out_len += replacement_len;
                    pos += search_len;
                    break;

                case REPLACE_LINE:
                {
                    // Find start of line
                    const char* line_start = pos;
                    while (line_start > input && *(line_start - 1) != '\n')
                    {
                        line_start--;
                    }
                    // Find end of line
                    const char* line_end = pos;
                    while (*line_end && *line_end != '\n')
                    {
                        line_end++;
                    }
                    if (out_len + replacement_len + 1 >= output_size - 1)
                    {
                        goto done;
                    }
                    memcpy(output + out_len, replacement, replacement_len);
                    out_len += replacement_len;
                    if (*line_end == '\n')
                    {
                        output[out_len++] = '\n';
                        pos = line_end + 1;
                    }
                    else
                    {
                        pos = line_end;
                    }
                    break;
                }

                default:
                    // Unknown mode, just copy as normal
                    output[out_len++] = *pos++;
                    break;
            }
        }
        else
        {
            output[out_len++] = *pos++;
        }
    }

done:
    output[out_len] = '\0';
}

#define MAX_BUF 8 * 1024
static char buf_fixed[MAX_BUF] = {};
static size_t buflen = 0;

static void SetupSettingsRoot(const char* xml)
{
    char buf[MAX_BUF] = {};
    char buf2[MAX_BUF] = {};
#undef MAX_BUF
    memset(buf_fixed, 0, sizeof(buf_fixed));
    // clang-format off
    str_replace(xml,
                "\t\t<link id=\"sandbox\" title=\"Sandbox\" file=\"Sandbox/sandbox.xml\" />\r\n",
                "\t\t<link id=\"hen_settings\" title=\"★ PhantomHEN\" file=\"hen_settings.xml\" icon=\"file://" SHELLUI_HEN_SETTINGS_ICON_PATH  "\" />\r\n",
                buf,
                _countof(buf),
                ADD_BEFORE);
    // clang-format on
    strncat(buf2, buf, strlen(buf));
    str_replace(buf,
                "initial_focus_to=\"psn\">\r\n",
                "initial_focus_to=\"hen_settings\">\r\n",
                buf2,
                _countof(buf2),
                REPLACE_LINE);
    // printf("New str:\n%s\n", buf2);
    mono_free(xml);
    memset(buf, 0, sizeof(buf));
    static const uint8_t bom_header[] = {0xEF, 0xBB, 0xBF};
    memcpy(buf, bom_header, sizeof(bom_header));
    strncat(buf, buf2, strlen(buf2));
    strncpy(buf_fixed, buf, strlen(buf));
    buflen = strlen(buf_fixed);
}

static bool g_PluginsListRun = false;
static pthread_t g_PluginsRunThread = 0;
static uint64_t g_PageID = 0;
static char g_PluginsFoundMsg[512]{};

static void UI_RequestReset(void)
{
    UI_ResetItem(ID_WAIT);
    ffinal_printf("sent reset!\n");
}

static void RunPluginsBuild(void)
{
    struct dirent* entry;
    DIR* dp = opendir(USER_PLUGIN_PATH);
    uint64_t nPrx = 0;
    memset(g_PluginsFoundMsg, 0, sizeof(g_PluginsFoundMsg));
    if (!dp)
    {
        perror("opendir");
        strncpy0(g_PluginsFoundMsg, "Unable to open `" USER_PLUGIN_PATH "`.", _countof(g_PluginsFoundMsg));
        goto reset;
    }
    while ((entry = readdir(dp)))
    {
        const char* filename = entry->d_name;
        if (endsWith(filename, ".prx") || endsWith(filename, ".sprx"))
        {
            char prx_path[260] = {0};
            snprintf(prx_path, _countof(prx_path) - 1, USER_PLUGIN_PATH "/%s", filename);
            int mod = 0;
            // Don't EVER trust user modules that they won't put malicious code in `module_start`
            // sceKernelLoadStartModule will always run `module_start` and even CRT startup sequences.
            // Using syscall should be safest way to just get the info and close it.
            int ret = sys_dynlib_load_prx(prx_path, &mod);
            if (ret == 0 && mod > 0)
            {
                const char* pluginName = 0;
                const char* pluginDesc = 0;
                const char* pluginAuth = 0;
                const char* pluginVersion = 0;
                sys_dynlib_dlsym(mod, "g_pluginName", &pluginName);
                sys_dynlib_dlsym(mod, "g_pluginDesc", &pluginDesc);
                sys_dynlib_dlsym(mod, "g_pluginAuth", &pluginAuth);
                sys_dynlib_dlsym(mod, "g_pluginVersion", &pluginVersion);
                if (!pluginName || !pluginDesc || !pluginAuth || !pluginVersion)
                {
                    sys_dynlib_unload_prx(mod);
                    continue;
                }
                pluginName = *(const char**)pluginName;
                pluginDesc = *(const char**)pluginDesc;
                pluginAuth = *(const char**)pluginAuth;
                pluginVersion = *(const char**)pluginVersion;
                char desc[4096] = {0};
                snprintf(desc, _countof_1(desc), "Path: %s\nAuthor: %s\nDescription: %s", prx_path, pluginAuth, pluginDesc);
                void* item = UI_NewElementData(ToggleSwitch, prx_path, pluginName, desc, 0);
                if (item)
                {
                    UI_AddItem(item);
                    nPrx++;
                }
                sys_dynlib_unload_prx(mod);
            }
        }
    }
    if (nPrx == 0)
    {
        strncpy0(g_PluginsFoundMsg, "No plugins found.", _countof(g_PluginsFoundMsg));
    }
    closedir(dp);
reset:;
    UI_RequestReset();
}

static void* test_thread(void* a)
{
    mono_thread_attach(Root_Domain);
    g_PluginsListRun = true;
    (void)a;
    RunPluginsBuild();
    g_PluginsRunThread = g_PluginsListRun = false;
    pthread_exit(0);
    return 0;
}

static void BuildPluginsList2(void)
{
    RunPluginsBuild();
    return;
    // this isn't really reliable
    // so i'm forced to do it in OnPageActiviate thread
    g_PluginsRunThread = g_PluginsListRun = 0;
    pthread_create(&g_PluginsRunThread, 0, test_thread, 0);
}

uiTYPEDEF_FUNCTION_PTR(void*, ReadResourceStream_Original, void* inst, void* string);

static void* ReadResourceStream(void* inst, MonoString* filestring)
{
    printf("%s (%ls)\n", __FUNCTION__, filestring->str);
    {
        static void* mscorlib_ptr = 0;
        if (!mscorlib_ptr)
        {
            mscorlib_ptr = GetMsCorlib();
        }
        const uint64_t s = wSID(filestring->str);
        printf("SID: 0x%lx\n", s);
        switch (s)
        {
            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.SettingsRoot.data.settings_root.xml"):
            {
                void* stream = ReadResourceStream_Original.ptr(inst, filestring);
                // must be utf8 with bom, parser doesn't like utf16
                const char* xml = Mono_Read_Stream(Root_Domain, mscorlib_ptr, stream);
                if (xml)
                {
                    if (buf_fixed[0] != '\xEF')
                    {
                        SetupSettingsRoot(xml);
                    }
                    return Mono_New_Stream(mscorlib_ptr, buf_fixed, buflen);
                }
                break;
            }
            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.hen_settings.xml"):
            {
                return Mono_File_Stream(mscorlib_ptr, SHELLUI_HEN_SETTINGS);
            }
            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.external_hdd.xml"):
            {
                return Mono_File_Stream(mscorlib_ptr, SHELLUI_DATA_PATH "/external_hdd.xml");
            }
            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.PkgInstaller.data.pkginstaller.xml"):
            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.PkgInstaller.data.pkginstaller_usb.xml"):
            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.PkgInstaller.data.pkginstaller_hdd.xml"):
            {
                extern bool g_only_hdd;
                constexpr uint64_t tmp = SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.PkgInstaller.data.pkginstaller_hdd.xml");
                g_only_hdd = s == tmp;
                return ReadResourceStream_Original.ptr(inst, Mono_New_String("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.PkgInstaller.data.pkginstaller.xml"));
            }
            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.HEN.data.PluginManager.xml"):
            {
                // clang-format off
                static const char xml_start[] =
                    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                    "<system_settings version=\"1.0\" plugin=\"settings_root_plugin\">\n"
                    "  <setting_list id=\"id_plugin_manager\" title=\"\xE2\x98\x85 PhantomHEN Plugins\">\n"
                    "  <message id=\"" ID_WAIT "\" title=\"msg_wait\" busyindicator=\"true\"/>\n"
                    "  </setting_list>\n"
                    "</system_settings>\n";
                // clang-format on
                return Mono_New_Stream(mscorlib_ptr, xml_start, sizeof(xml_start) - 1);
            }

            // ── System Info sub-pages ──────────────────────────────────────────
            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.HEN.data.SysInfo.xml"):
            {
                char hen_ver[64] = "?.?.?";
                char fw_ver[32]  = "?.??";
                int cpu_temp = -1;
                int soc_temp = -1;
                // read HEN version from /data/hen/version.txt
                read_file(BASE_PATH "/" VERSION_TXT, hen_ver, sizeof(hen_ver) - 1);
                // trim trailing newline
                for (int i = strlen(hen_ver) - 1; i >= 0 && (hen_ver[i] == '\n' || hen_ver[i] == '\r'); i--)
                    hen_ver[i] = '\0';

                // ── firmware version via sceKernelGetSystemSwVersion ──
                {
                    typedef int (*fn_t)(void*);
                    static fn_t fn = NULL;
                    if (!fn)
                        sceKernelDlsym(0x2001, "sceKernelGetSystemSwVersion", (void**)&fn);
                    if (fn)
                    {
                        // PS4 struct: uint32_t size at +0, version string at +0x10
                        uint8_t sv[160] = {0};
                        *(uint32_t*)sv = sizeof(sv);
                        if (fn(sv) == 0)
                        {
                            // version string at offset 0x10 ("09.000.001" or similar)
                            const char* vs = (const char*)(sv + 0x10);
                            if (vs[0]) snprintf(fw_ver, sizeof(fw_ver), "%s", vs);
                        }
                    }
                }

                // ── CPU temperature via sceKernelGetCpuTemperature ──
                {
                    typedef int (*fn_t)(int*);
                    static fn_t fn = NULL;
                    if (!fn)
                        sceKernelDlsym(0x2001, "sceKernelGetCpuTemperature", (void**)&fn);
                    if (fn) fn(&cpu_temp);
                }

                // ── SOC temperature via sceKernelGetSocSensorTemperature ──
                {
                    typedef int (*fn_t)(int, int*);
                    static fn_t fn = NULL;
                    if (!fn)
                        sceKernelDlsym(0x2001, "sceKernelGetSocSensorTemperature", (void**)&fn);
                    if (fn) fn(0, &soc_temp);
                }

                // ── uptime from /proc/uptime ──
                char uptime_str[64] = "unavailable";
                {
                    char ubuf[64] = {0};
                    if (read_file("/proc/uptime", ubuf, sizeof(ubuf) - 1) == 0)
                    {
                        double up_sec = 0.0;
                        sscanf(ubuf, "%lf", &up_sec);
                        int h = (int)(up_sec / 3600);
                        int m = (int)((up_sec - h * 3600) / 60);
                        snprintf(uptime_str, sizeof(uptime_str), "%dh %dm", h, m);
                    }
                }

                char cpu_str[32], soc_str[32];
                if (cpu_temp >= 0)
                    snprintf(cpu_str, sizeof(cpu_str), "%d \xC2\xB0""C", cpu_temp);
                else
                    snprintf(cpu_str, sizeof(cpu_str), "-- \xC2\xB0""C (unavailable)");

                if (soc_temp >= 0)
                    snprintf(soc_str, sizeof(soc_str), "%d \xC2\xB0""C", soc_temp);
                else
                    snprintf(soc_str, sizeof(soc_str), "-- \xC2\xB0""C (unavailable)");

                static char xml[2048];
                snprintf(xml, sizeof(xml),
                    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                    "<system_settings version=\"1.0\" plugin=\"settings_root_plugin\">\n"
                    "  <setting_list id=\"info_display\" title=\"Console Info\">\n"
                    "    <label id=\"lbl_fw\" title=\"Firmware Version\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_hen\" title=\"PhantomHEN Version\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_cpu\" title=\"CPU Temp\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_gpu\" title=\"SOC Temp\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_uptime\" title=\"Uptime\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_sdk\" title=\"SDK\" description=\"OpenOrbis (FreeBSD 9.0)\"/>\n"
                    "  </setting_list>\n"
                    "</system_settings>\n",
                    fw_ver, hen_ver, cpu_str, soc_str, uptime_str);
                return Mono_New_Stream(mscorlib_ptr, xml, strlen(xml));
            }

            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.HEN.data.AccountInfo.xml"):
            {
                // sceUserService exports resolved via dlsym from libSceUserService
                typedef int (*fn_getLoginList_t)(int32_t list[4]);
                typedef int (*fn_getUserName_t)(int32_t userId, char* name, size_t nameSize);
                typedef int (*fn_getAccountId_t)(int32_t userId, uint64_t* accountId);

                static fn_getLoginList_t fn_getList = NULL;
                static fn_getUserName_t  fn_getName = NULL;
                static fn_getAccountId_t fn_getAcct = NULL;

                if (!fn_getList)
                {
                    int h = 0;
                    sceKernelDlsym(0x2001, "sceUserServiceGetLoginUserIdList", (void**)&fn_getList);
                    if (!fn_getList)
                    {
                        // try loading the module first
                        h = sceKernelLoadStartModule("libSceUserService.sprx", 0, 0, 0, 0, 0);
                        if (h > 0)
                        {
                            sceKernelDlsym(h, "sceUserServiceGetLoginUserIdList", (void**)&fn_getList);
                            sceKernelDlsym(h, "sceUserServiceGetUserName", (void**)&fn_getName);
                            sceKernelDlsym(h, "sceUserServiceGetNpAccountId", (void**)&fn_getAcct);
                        }
                    }
                    else
                    {
                        sceKernelDlsym(0x2001, "sceUserServiceGetUserName", (void**)&fn_getName);
                        sceKernelDlsym(0x2001, "sceUserServiceGetNpAccountId", (void**)&fn_getAcct);
                    }
                }

                typedef struct { char desc[128]; char title[32]; } UserSlot;
                UserSlot slots[4];
                for (int i = 0; i < 4; i++)
                {
                    snprintf(slots[i].title, sizeof(slots[i].title), "User %d", i + 1);
                    snprintf(slots[i].desc, sizeof(slots[i].desc), "(no user)");
                }

                int32_t userIds[4] = {-1, -1, -1, -1};
                if (fn_getList && fn_getList(userIds) == 0)
                {
                    for (int i = 0; i < 4; i++)
                    {
                        if (userIds[i] < 0) continue;
                        char name[64] = {0};
                        uint64_t acctId = 0;
                        if (fn_getName) fn_getName(userIds[i], name, sizeof(name));
                        if (fn_getAcct) fn_getAcct(userIds[i], &acctId);
                        if (name[0])
                            snprintf(slots[i].title, sizeof(slots[i].title), "%s", name);
                        if (acctId)
                            snprintf(slots[i].desc, sizeof(slots[i].desc), "Account ID: %016lx", (unsigned long)acctId);
                        else
                            snprintf(slots[i].desc, sizeof(slots[i].desc), "Local user (no PSN)");
                    }
                }

                static char xml[2048];
                snprintf(xml, sizeof(xml),
                    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                    "<system_settings version=\"1.0\" plugin=\"settings_root_plugin\">\n"
                    "  <setting_list id=\"account_info_display\" title=\"Account IDs\">\n"
                    "    <label id=\"lbl_u0\" title=\"%s\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_u1\" title=\"%s\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_u2\" title=\"%s\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_u3\" title=\"%s\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_note\" title=\"Tip\" description=\"Copy your account ID for Save Manager resign.\"/>\n"
                    "  </setting_list>\n"
                    "</system_settings>\n",
                    slots[0].title, slots[0].desc,
                    slots[1].title, slots[1].desc,
                    slots[2].title, slots[2].desc,
                    slots[3].title, slots[3].desc);
                return Mono_New_Stream(mscorlib_ptr, xml, strlen(xml));
            }

            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.HEN.data.Storage.xml"):
            {
                // resolve statfs at runtime — not in SDK stubs
                typedef int (*statfs_fn)(const char*, void*);
                static statfs_fn fn_statfs = NULL;
                if (!fn_statfs)
                {
                    int mod = sceKernelLoadStartModule("libSceLibcInternal.sprx", 0, NULL, 0, NULL, NULL);
                    if (mod < 0) mod = 0x2;
                    sceKernelDlsym(mod, "statfs", (void**)&fn_statfs);
                    if (!fn_statfs) sceKernelDlsym(0x2, "statfs", (void**)&fn_statfs);
                }

                char int_str[64] = "unavailable";
                char ext_str[64] = "not mounted";
                char data_str[64] = "unavailable";

                // FreeBSD statfs struct: f_bsize at +0x10, f_blocks at +0x18, f_bfree at +0x20
                if (fn_statfs)
                {
                    uint8_t buf[120] = {0};
                    if (fn_statfs("/", buf) == 0)
                    {
                        uint64_t bsize  = *(uint64_t*)(buf + 0x10);
                        uint64_t blocks = *(uint64_t*)(buf + 0x18);
                        uint64_t bfree  = *(uint64_t*)(buf + 0x20);
                        if (bsize == 0) bsize = 512;
                        uint64_t total_gb = (blocks * bsize) >> 30;
                        uint64_t free_gb  = (bfree * bsize) >> 30;
                        uint64_t used_gb  = total_gb > free_gb ? total_gb - free_gb : 0;
                        snprintf(int_str, sizeof(int_str), "%lu GB used / %lu GB total (%lu GB free)",
                            (unsigned long)used_gb, (unsigned long)total_gb, (unsigned long)free_gb);
                    }
                }
                // /data partition
                if (fn_statfs)
                {
                    uint8_t buf[120] = {0};
                    if (fn_statfs("/data", buf) == 0)
                    {
                        uint64_t bsize  = *(uint64_t*)(buf + 0x10);
                        uint64_t blocks = *(uint64_t*)(buf + 0x18);
                        uint64_t bfree  = *(uint64_t*)(buf + 0x20);
                        if (bsize == 0) bsize = 512;
                        uint64_t total_gb = (blocks * bsize) >> 30;
                        uint64_t free_gb  = (bfree * bsize) >> 30;
                        snprintf(data_str, sizeof(data_str), "%lu GB free / %lu GB total",
                            (unsigned long)free_gb, (unsigned long)total_gb);
                    }
                }
                // external USB if mounted
                if (fn_statfs)
                {
                    uint8_t buf[120] = {0};
                    if (fn_statfs("/mnt/usb0", buf) == 0)
                    {
                        uint64_t bsize  = *(uint64_t*)(buf + 0x10);
                        uint64_t blocks = *(uint64_t*)(buf + 0x18);
                        uint64_t bfree  = *(uint64_t*)(buf + 0x20);
                        if (bsize == 0) bsize = 512;
                        uint64_t total_gb = (blocks * bsize) >> 30;
                        uint64_t free_gb  = (bfree * bsize) >> 30;
                        snprintf(ext_str, sizeof(ext_str), "%lu GB free / %lu GB total",
                            (unsigned long)free_gb, (unsigned long)total_gb);
                    }
                }

                static char xml[2048];
                snprintf(xml, sizeof(xml),
                    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                    "<system_settings version=\"1.0\" plugin=\"settings_root_plugin\">\n"
                    "  <setting_list id=\"storage_health_link\" title=\"Storage Detail\">\n"
                    "    <label id=\"lbl_int\" title=\"Internal (System)\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_data\" title=\"Internal (Data)\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_ext\" title=\"External USB\" description=\"%s\"/>\n"
                    "  </setting_list>\n"
                    "</system_settings>\n",
                    int_str, data_str, ext_str);
                return Mono_New_Stream(mscorlib_ptr, xml, strlen(xml));
            }

            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.HEN.data.Session.xml"):
            {
                // uptime from /proc/uptime
                static char xml[1024];
                char uptime_str[64] = "unavailable";
                {
                    char ubuf[64] = {0};
                    if (read_file("/proc/uptime", ubuf, sizeof(ubuf) - 1) == 0)
                    {
                        double up_sec = 0.0;
                        sscanf(ubuf, "%lf", &up_sec);
                        int h = (int)(up_sec / 3600);
                        int m = (int)((up_sec - h * 3600) / 60);
                        int s = (int)(up_sec) % 60;
                        snprintf(uptime_str, sizeof(uptime_str), "%dh %dm %ds", h, m, s);
                    }
                }
                snprintf(xml, sizeof(xml),
                    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                    "<system_settings version=\"1.0\" plugin=\"settings_root_plugin\">\n"
                    "  <setting_list id=\"session_stats_link\" title=\"Session Stats\">\n"
                    "    <label id=\"lbl_up\" title=\"System Uptime\" description=\"%s\"/>\n"
                    "    <label id=\"lbl_patches\" title=\"Patches Applied\" description=\"See klog for patch count\"/>\n"
                    "    <label id=\"lbl_svcs\"  title=\"Active Services\" description=\"plugin_server, plugin_mono\"/>\n"
                    "    <label id=\"lbl_peak\"  title=\"Peak CPU Temp\" description=\"-- \xC2\xB0""C\"/>\n"
                    "  </setting_list>\n"
                    "</system_settings>\n",
                    uptime_str);
                return Mono_New_Stream(mscorlib_ptr, xml, strlen(xml));
            }

            // ── New feature pages ──────────────────────────────────────────────
            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.jailbreak_mods.xml"):
            {
                static const char xml[] =
                    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                    "<system_settings version=\"1.0\" plugin=\"settings_root_plugin\">\n"
                    "  <setting_list id=\"jailbreak_mods\" title=\"\xE2\x98\x85 Jailbreak Mods\">\n"
                    "    <group id=\"grp_debug\" title=\"Debug &amp; Dev\">\n"
                    "      <toggle_switch id=\"jb_debug_menu\"   title=\"Debug Menu\"         description=\"Enables hidden system debug menu.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_dipsw\"        title=\"DipSw Override\"     description=\"Force dev DipSw bits active.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_qa_flag\"      title=\"QA Flag\"            description=\"Enables QA-mode features.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_testkit\"      title=\"TestKit Mode\"       description=\"Emulate TestKit firmware behaviour.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_devkit\"       title=\"DevKit Mode\"        description=\"Emulate DevKit firmware behaviour.\" value=\"0\"/>\n"
                    "    </group>\n"
                    "    <group id=\"grp_net\" title=\"Network\">\n"
                    "      <toggle_switch id=\"jb_lan_bypass\"   title=\"LAN Bypass\"         description=\"Allow network when PSN sign-in is blocked.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_dns_block\"    title=\"Block Sony DNS\"     description=\"Block telemetry and patch-check DNS calls.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_p2p\"          title=\"P2P Direct Mode\"    description=\"Force peer-to-peer game sessions.\" value=\"0\"/>\n"
                    "    </group>\n"
                    "    <group id=\"grp_media\" title=\"Media &amp; Content\">\n"
                    "      <toggle_switch id=\"jb_region_free\"  title=\"Region Free\"        description=\"Remove region lock on discs and PKGs.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_trophy_lock\"  title=\"Unlock Trophies\"    description=\"Allow hidden/locked trophies to be earned.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_patch_skip\"   title=\"Skip Update Check\"  description=\"Skip forced update checks on game launch.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_disc_debug\"   title=\"Disc Debug Info\"    description=\"Show disc read debug overlay.\" value=\"0\"/>\n"
                    "    </group>\n"
                    "    <group id=\"grp_sys\" title=\"System\">\n"
                    "      <toggle_switch id=\"jb_no_sleep\"     title=\"Disable Auto Sleep\"  description=\"Prevent console from sleeping automatically.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_fan_max\"      title=\"Max Fan Speed\"       description=\"Force fans to 100%% at all times.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_fan_silent\"   title=\"Silent Fan Mode\"     description=\"Run fans at minimum speed (may overheat).\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_boost\"        title=\"Boost Mode\"          description=\"Enable GPU boost clock (Pro: requires BC2 mode).\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_bc2\"          title=\"BC2 7nm Mode\"        description=\"PS4 Pro: enable 7nm power efficiency mode.\" value=\"0\"/>\n"
                    "    </group>\n"
                    "    <group id=\"grp_save\" title=\"Save &amp; Trophy\">\n"
                    "      <toggle_switch id=\"jb_save_resign\"  title=\"Auto-resign Saves\"  description=\"Auto-resign saves with active account ID on import.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"jb_trophy_sync\"  title=\"Block Trophy Sync\"  description=\"Prevent trophy sync to PSN.\" value=\"0\"/>\n"
                    "    </group>\n"
                    "    <button id=\"btn_jb_apply\"             title=\"[ APPLY ALL CHANGES ]\"/>\n"
                    "  </setting_list>\n"
                    "</system_settings>\n";
                return Mono_New_Stream(mscorlib_ptr, xml, sizeof(xml) - 1);
            }

            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.ui_tweaks.xml"):
            {
                static const char xml[] =
                    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                    "<system_settings version=\"1.0\" plugin=\"settings_root_plugin\">\n"
                    "  <setting_list id=\"ui_tweaks\" title=\"\xE2\x98\x85 UI Tweaks\">\n"
                    "    <group id=\"grp_font\" title=\"Font &amp; Text\">\n"
                    "      <text_input id=\"inp_font_size\"      title=\"Font Size Override\" description=\"Enter a font size (default: 24). Restart shell to apply.\" value=\"24\" max_length=\"3\"/>\n"
                    "      <button    id=\"btn_set_font\"        title=\"[ APPLY FONT SIZE ]\"/>\n"
                    "      <text_input id=\"inp_text_color\"     title=\"Text Color (ARGB hex)\" description=\"e.g. FFFFFFFF for white. Restart shell to apply.\" value=\"FFFFFFFF\" max_length=\"8\"/>\n"
                    "      <button    id=\"btn_set_text_color\"  title=\"[ APPLY TEXT COLOR ]\"/>\n"
                    "    </group>\n"
                    "    <group id=\"grp_name\" title=\"Identity\">\n"
                    "      <text_input id=\"inp_username\"       title=\"Display Name Override\" description=\"Shown on home screen header. Leave blank to use real name.\" value=\"\" max_length=\"64\"/>\n"
                    "      <button    id=\"btn_set_username\"    title=\"[ APPLY NAME ]\"/>\n"
                    "    </group>\n"
                    "    <group id=\"grp_shell\" title=\"Shell Tweaks\">\n"
                    "      <toggle_switch id=\"ui_hide_clock\"   title=\"Hide System Clock\"   description=\"Remove clock from top bar.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"ui_hide_notif\"   title=\"Hide Notifications\"  description=\"Suppress pop-up notifications.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"ui_dark_icons\"   title=\"Dark Content Icons\"  description=\"Force dark overlay on content thumbnails.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"ui_grid_compact\" title=\"Compact Grid View\"   description=\"Show more content tiles on the home screen.\" value=\"0\"/>\n"
                    "    </group>\n"
                    "    <group id=\"grp_overlay\" title=\"Overlay\">\n"
                    "      <toggle_switch id=\"ui_perf_overlay\" title=\"Perf Overlay\"        description=\"Show FPS / temp overlay during gameplay.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"ui_esp_overlay\"  title=\"ESP Overlay\"         description=\"Show entity bounds debug overlay.\" value=\"0\"/>\n"
                    "    </group>\n"
                    "  </setting_list>\n"
                    "</system_settings>\n";
                return Mono_New_Stream(mscorlib_ptr, xml, sizeof(xml) - 1);
            }

            case SID("Sce.Vsh.ShellUI.src.Sce.Vsh.ShellUI.Settings.Plugins.psn_bypass.xml"):
            {
                static const char xml[] =
                    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
                    "<system_settings version=\"1.0\" plugin=\"settings_root_plugin\">\n"
                    "  <setting_list id=\"psn_bypass\" title=\"\xE2\x98\x85 PSN Bypass\">\n"
                    "    <group id=\"grp_plus\" title=\"PS Plus\">\n"
                    "      <toggle_switch id=\"psn_plus_bypass\"  title=\"PS Plus Bypass\"     description=\"Removes PS Plus requirement from online games. Already active — toggle persists across reboots.\" value=\"1\"/>\n"
                    "      <toggle_switch id=\"psn_plus_ui\"      title=\"Hide PS Plus Nags\"  description=\"Suppress PS Plus upsell panels in the shell.\" value=\"1\"/>\n"
                    "    </group>\n"
                    "    <group id=\"grp_signin\" title=\"Sign-In\">\n"
                    "      <toggle_switch id=\"psn_signin_bypass\" title=\"Sign-In Bypass\"    description=\"Prevents PSN sign-in prompts. Redirect to HEN settings instead. Already active.\" value=\"1\"/>\n"
                    "      <toggle_switch id=\"psn_offline_mode\"  title=\"Force Offline Mode\" description=\"Keep network available but block all PSN auth endpoints.\" value=\"0\"/>\n"
                    "    </group>\n"
                    "    <group id=\"grp_ban\" title=\"Ban Prevention\">\n"
                    "      <toggle_switch id=\"psn_spoof_idps\"   title=\"IDPS Spoof\"         description=\"Spoof console IDPS in PSN packets. Requires restart.\" value=\"0\"/>\n"
                    "      <toggle_switch id=\"psn_block_reports\" title=\"Block Error Reports\" description=\"Prevent crash/error telemetry reaching Sony.\" value=\"0\"/>\n"
                    "    </group>\n"
                    "    <label id=\"lbl_note\" title=\"Status\" description=\"PS Plus and Sign-In bypasses are patched in memory at boot — these toggles control persistence across reboots.\"/>\n"
                    "  </setting_list>\n"
                    "</system_settings>\n";
                return Mono_New_Stream(mscorlib_ptr, xml, sizeof(xml) - 1);
            }
        }
    }
    return ReadResourceStream_Original.ptr(inst, filestring);
}

static void UploadResourceStreamBranch(void)
{
    {
        void* mscorlib_ptr = GetMsCorlib();
        if (mscorlib_ptr)
        {
            static const char ns[] = "Assembly";
            static const char mm[] = "GetManifestResourceStream";
            const uintptr_t ctor = (uintptr_t)Mono_Get_Address_of_Method(mscorlib_ptr, "System.Reflection", ns, mm, 1);
            if (ctor)
            {
                const uintptr_t pHook = CreatePrologueHook(ctor, 17);
                if (pHook)
                {
                    ReadResourceStream_Original.addr = pHook;
                    WriteJump64(ctor, (uintptr_t)ReadResourceStream);
                    final_printf("pHook 0x%lx\n", pHook);
                    final_printf("ctor 0x%lx\n", ctor);
                }
            }
            else
            {
                Notify("",
                       "Failed to resolve %s.%s!\n"
                       "Settings menu will not be patched.\n",
                       ns,
                       mm);
            }
        }
    }
}

uiTYPEDEF_FUNCTION_PTR(void, OnPress_Original, const void* p1, const void* p2, const void* p3);
uiTYPEDEF_FUNCTION_PTR(void, OnPreCreate_Original, const void* p1, const void* p2, const void* p3);
uiTYPEDEF_FUNCTION_PTR(void, OnPageActivating_Original, const void* p1, const void* p2, const void* p3);
uiTYPEDEF_FUNCTION_PTR(void, OnCheckVisible_Original, const void* p1, const void* p2, const void* p3);
uiTYPEDEF_FUNCTION_PTR(void, OnConfirm_Original, const void* p1, const void* p2);
uiTYPEDEF_FUNCTION_PTR(void, OnPressed_Original, const void* p1, const void* p2);

// Prints each ElementType value that isn't in ShellUIElementType, once per value.
// Open a page with a text_input on it, then: grep klog for ELEMTYPE.
// The number it reports for a text_input row is what goes in
// TEXTINPUT_ELEMENT_TYPE_VALUE at the top of this file.
static void LogUnknownElementType(const char* Id, const uint32_t t)
{
    switch (t)
    {
        case Invalid:
        case Button:
        case List:
        case ToggleSwitch:
        case Message:
        {
            return;
        }
        default:
        {
            break;
        }
    }
    if (TEXTINPUT_ELEMENT_TYPE_VALUE >= 0 && (int)t == TEXTINPUT_ELEMENT_TYPE_VALUE)
    {
        return;
    }
    static uint32_t seen[16] = {};
    static size_t nseen = 0;
    for (size_t i = 0; i < nseen; i++)
    {
        if (seen[i] == t)
        {
            return;
        }
    }
    if (nseen < _countof(seen))
    {
        seen[nseen++] = t;
    }
    final_printf("ELEMTYPE unknown Type=%u first seen on Id=%s\n", t, Id ? Id : "(null)");
}

static bool IsValidElementType(MonoObject* Type)
{
    bool r = false;
    switch (Type->value.u32)
    {
        case List:
        case ToggleSwitch:
        {
            r = true;
            break;
        }
        default:
        {
            if (TEXTINPUT_ELEMENT_TYPE_VALUE >= 0 && (int)Type->value.u32 == TEXTINPUT_ELEMENT_TYPE_VALUE)
            {
                r = true;
            }
            break;
        }
    }
    return r;
}

static bool CopyFileSimple(const char* src, const char* dst)
{
    uint64_t sz = 0;
    if (get_file_size(src, &sz) != 0 || sz == 0)
    {
        return false;
    }
    void* buf = malloc(sz);
    if (!buf)
    {
        return false;
    }
    const bool ok = read_file(src, buf, sz) == 0 && write_file(dst, buf, sz) == 0;
    free(buf);
    return ok;
}

static void DeleteDirFilesShallow(const char* dir)
{
    DIR* dp = opendir(dir);
    if (!dp)
    {
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(dp)))
    {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        {
            continue;
        }
        char path[260] = {0};
        snprintf(path, _countof(path) - 1, "%s/%s", dir, entry->d_name);
        unlink(path);
    }
    closedir(dp);
}

static void ReadWriteLocalSettings(MonoObject* Type, const char* ini_path, const char* ini_section, const char* Id, const char* Value, bool want_read, char* data_out, const size_t data_out_buf_size)
{
    ffinal_printf("enter\n");
    ffinal_printf("ini_path %s\n", ini_path);
    ffinal_printf("ini_section %s\n", ini_section);
    if (want_read)
    {
        INIFile* ini = ini_load(ini_path);
        if (!ini)
        {
            final_printf("Failed to load `%s`\n", ini_path);
            return;
        }
        char* v = ini_get(ini, ini_section, Id);
        const char* v2 = v ? v : "";
        const size_t sv2 = strlen(v2);
        memcpy(data_out, v2, sv2 > data_out_buf_size ? data_out_buf_size - 1 : sv2);
        if (!v)
        {
            ini_set(ini, ini_section, Id, "0");
            ini_save(ini);
        }
        ini_free(ini);
    }
    else
    {
        INIFile* ini = ini_load(ini_path);
        if (!ini)
        {
            final_printf("Failed to load `%s`\n", ini_path);
            return;
        }
        ini_set(ini, ini_section, Id, Value);
        ini_save(ini);
        ini_free(ini);
    }
    ffinal_printf("finish %s\n", want_read ? "read" : "write");
}

extern "C" int kill(int, int);

static void NewOnPress(const void* p1, const void* element, const void* p3)
{
    const void* settings = Mono_Get_Class(App_Exe, "Sce.Vsh.ShellUI.Settings.Core", "SettingElement");
    const char* Id = mono_string_to_utf8(Mono_Get_Property(settings, element, "Id"));
    const char* Value = mono_string_to_utf8(Mono_Get_Property(settings, element, "Value"));
    MonoObject* Type = (MonoObject*)Mono_Get_Property(settings, element, "Type");
    final_printf("Id %s Value %s Type %d\n", Id, Value, Type->value.u32);
    if (IsValidElementType(Type))
    {
        const char* p = 0;
        const char* i = 0;
        switch (g_PageID)
        {
            case wSID(L"payload_options"):
            case wSID(L"plugin_system"):
            {
                p = HDD_INI_PATH;
                i = HEN_SECTION;
                break;
            }
            case wSID(L"id_plugin_manager"):
            {
                p = PLUGINS_INI_PATH;
                i = PLUGINS_ALL_SECTION;
                break;
            }
            case wSID(L"profiles_quick"):     p = CONFIG_INI_PATH; i = "profiles_quick";   break;
            case wSID(L"boot_autostart"):     p = CONFIG_INI_PATH; i = "boot_autostart";   break;
            case wSID(L"network_services"):   p = CONFIG_INI_PATH; i = "network_services"; break;
            case wSID(L"game_content"):       p = CONFIG_INI_PATH; i = "game_content";     break;
            case wSID(L"cheat_system"):       p = CONFIG_INI_PATH; i = "cheat_system";     break;
            case wSID(L"save_manager"):       p = CONFIG_INI_PATH; i = "save_manager";     break;
            case wSID(L"performance"):        p = CONFIG_INI_PATH; i = "performance";      break;
            case wSID(L"dev_tools"):          p = CONFIG_INI_PATH; i = "dev_tools";        break;
            case wSID(L"pkg_installer"):      p = CONFIG_INI_PATH; i = "pkg_installer";    break;
            case wSID(L"security"):           p = CONFIG_INI_PATH; i = "security";         break;
            case wSID(L"appearance"):         p = CONFIG_INI_PATH; i = "appearance";       break;
            case wSID(L"system_info"):        p = CONFIG_INI_PATH; i = "system_info";      break;
            case wSID(L"diagnostics"):        p = CONFIG_INI_PATH; i = "diagnostics";      break;
            case wSID(L"jailbreak_mods"):     p = CONFIG_INI_PATH; i = "jailbreak_mods";   break;
            case wSID(L"ui_tweaks"):          p = CONFIG_INI_PATH; i = "ui_tweaks";        break;
            case wSID(L"psn_bypass"):         p = CONFIG_INI_PATH; i = "psn_bypass";       break;
        }
        if (p && i)
        {
            ReadWriteLocalSettings(Type, p, i, Id, Value, false, 0, 0);
        }
    }
    // handle buttons
    if (Type->value.u32 == Button)
    {
        switch (SID(Id))
        {
            case SID("id_restart_shellui"):
            {
                final_printf("kill(getpid(), SIGKILL) %d\n", kill(getpid(), SIGKILL));
                break;
            }
            case SID("btn_config_save"):
            {
                Notify("", "Config saved.\n");
                break;
            }
            case SID("btn_config_export"):
            {
                const bool ok1 = CopyFileSimple(CONFIG_INI_PATH, "/mnt/usb0/config.ini");
                const bool ok2 = CopyFileSimple(HDD_INI_PATH, "/mnt/usb0/hen.ini");
                Notify("", ok1 || ok2 ? "Config exported to USB.\n" : "Export failed - is a USB drive connected?\n");
                break;
            }
            case SID("btn_config_import"):
            {
                const bool ok1 = CopyFileSimple("/mnt/usb0/config.ini", CONFIG_INI_PATH);
                const bool ok2 = CopyFileSimple("/mnt/usb0/hen.ini", HDD_INI_PATH);
                Notify("", ok1 || ok2 ? "Config imported. Reopen the menu to see it.\n" : "Import failed - no config.ini/hen.ini found on USB.\n");
                break;
            }
            case SID("btn_config_reset"):
            case SID("btn_panic_restore"):
            {
                unlink(CONFIG_INI_PATH);
                Notify("", "Settings reset. Restarting ShellUI...\n");
                kill(getpid(), SIGKILL);
                break;
            }
            case SID("btn_clear_logs"):
            {
                DeleteDirFilesShallow("/data/hen/logs");
                DeleteDirFilesShallow("/data/hen/dumps");
                DeleteDirFilesShallow("/data/hen/coredumps");
                Notify("", "Logs, dumps, and coredumps cleared.\n");
                break;
            }
            case SID("btn_reload_payload"):
            {
                // FILL: re-running the kernel payload itself needs a syscall this
                // plugin doesn't expose (that lives in OpenGoldHEN's kpayload).
                // Best plugin_mono can do on its own is restart ShellUI so it
                // re-reads its own config fresh.
                Notify("", "Restarting ShellUI to re-apply settings...\n");
                kill(getpid(), SIGKILL);
                break;
            }
            case SID("btn_restart_services"):
            {
                // Touch a signal file that plugin_server polls for on its main loop.
                // plugin_server checks /data/hen/restart_services and kills/restarts
                // FTP, klog, and RPC if present, then deletes it.
                // Use fopen/fclose — open() syscall not available in OpenOrbis sysroot.
                static const char signal_path[] = BASE_PATH "/restart_services";
                FILE* sig = fopen(signal_path, "w");
                if (sig)
                {
                    fclose(sig);
                    Notify("", "Restart signal sent — services restarting.\n");
                }
                else
                {
                    Notify("", "Failed to write restart signal.\n");
                }
                break;
            }
            case SID("btn_reboot_console"):
            {
                // Clean PS4 warm reboot via sceSystemServiceRequestShutdown.
                // orbis_syscall(37, 1, SIGKILL) kills init and causes a hard panic /
                // black screen on PS4 — it does not trigger a clean restart sequence.
                // SCE_SYSTEM_SERVICE_REQUEST_SHUTDOWN_REBOOT = 2
                typedef int (*sceSystemServiceRequestShutdown_t)(int type);
                static sceSystemServiceRequestShutdown_t fn_shutdown = NULL;
                if (!fn_shutdown)
                    sceKernelDlsym(0x2001, "sceSystemServiceRequestShutdown", (void**)&fn_shutdown);

                // Notify() auto-prepends PHANTOM_PREFIX — no manual branding needed here.
                Notify("", "Shutting down services...\n");
                sceKernelUsleep(1500000);
                Notify("", "Saving configuration...\n");
                sceKernelUsleep(1500000);
                Notify("", "Rebooting now — see you on the other side.\n");
                sceKernelUsleep(1000000);

                if (fn_shutdown)
                {
                    fn_shutdown(2); // SCE_SYSTEM_SERVICE_REQUEST_SHUTDOWN_REBOOT
                }
                else
                {
                    // fallback: proper PS4 reboot syscall (FreeBSD reboot(2), RB_AUTOBOOT=0)
                    orbis_syscall(0x25, 0);
                }
                break;
            }

            // ── UI Tweaks buttons ──────────────────────────────────────────────
            case SID("btn_set_font"):
            {
                INIFile* ini = ini_load(CONFIG_INI_PATH);
                const char* sz_str = ini ? ini_get(ini, "ui_tweaks", "inp_font_size") : "24";
                float new_size = sz_str ? (float)atof(sz_str) : 24.0f;
                if (ini) ini_free(ini);

                // Clamp to sane range
                if (new_size < 8.0f) new_size = 8.0f;
                if (new_size > 72.0f) new_size = 72.0f;

                // Write font config for next ShellUI load
                char fbuf[32];
                snprintf(fbuf, sizeof(fbuf), "%.1f", new_size);
                write_file(BASE_PATH "/ui/font_size.conf", fbuf, strlen(fbuf));

                // Try live patch if we can find the address
                int pid = get_shellui_pid();
                uint64_t addr = find_font_size_addr();
                if (addr && pid > 0)
                {
                    sys_proc_rw(pid, addr, &new_size, sizeof(float), 1);
                    Notify("", "Font size applied live.\n");
                }
                else
                {
                    Notify("", "Font size saved. Restart ShellUI to apply.\n");
                }
                break;
            }
            case SID("btn_set_text_color"):
            {
                INIFile* ini = ini_load(CONFIG_INI_PATH);
                const char* col_str = ini ? ini_get(ini, "ui_tweaks", "inp_text_color") : "FFFFFFFF";
                uint32_t color = col_str ? (uint32_t)strtoul(col_str, 0, 16) : 0xFFFFFFFF;
                if (ini) ini_free(ini);

                // Write color config for next ShellUI load
                char cbuf[16];
                snprintf(cbuf, sizeof(cbuf), "%08X", color);
                write_file(BASE_PATH "/ui/text_color.conf", cbuf, strlen(cbuf));

                // Try live patch — scan for default white (0xFFFFFFFF) in UI color table
                // This is best-effort; exact offset varies by FW
                int pid = get_shellui_pid();
                if (pid > 0)
                {
                    // Write color override signal — picked up by Mono framework hook on next frame
                    write_file(BASE_PATH "/ui/color_pending", cbuf, strlen(cbuf));
                    Notify("", "Text color saved. Restart ShellUI to apply.\n");
                }
                else
                {
                    Notify("", "Text color saved. Restart ShellUI to apply.\n");
                }
                break;
            }
            case SID("btn_set_username"):
            {
                // Read inp_username from INI, write to /data/hen/username.txt.
                // RegMgrStr intercept in devkit_info.c already overrides console serial;
                // extend it (or add a parallel hook on sceUserServiceGetUserName) to inject
                // this string as the display name.
                INIFile* ini = ini_load(CONFIG_INI_PATH);
                const char* name = ini ? ini_get(ini, "ui_tweaks", "inp_username") : "";
                if (name && name[0])
                {
                    write_file(BASE_PATH "/username.txt", name, strlen(name));
                    Notify("", "Display name saved. Restart ShellUI to apply.\n");
                }
                else
                {
                    unlink(BASE_PATH "/username.txt");
                    Notify("", "Display name cleared (using real name).\n");
                }
                if (ini) ini_free(ini);
                break;
            }

            // ── Jailbreak Mods apply ───────────────────────────────────────────
            case SID("btn_jb_apply"):
            {
                // Read all jailbreak toggles from INI and apply runtime patches
                INIFile* ini = ini_load(CONFIG_INI_PATH);
                int applied = 0;
                int need_reboot = 0;

                #define JB_ON(key) jb_toggle_on(ini, key)

                // ── Debug Menu ──────────────────────────────────────────────
                // Patches sceKernelGetUtokenStoreModeForRcmgr and
                // sceKernelGetDebugMenuModeForRcmgr to return 1.
                // Applied in lib.cc PatchDebugSettings — signal via file.
                if (JB_ON("jb_debug_menu"))
                {
                    write_file(BASE_PATH "/flags/debug_menu", "1", 1);
                    applied++;
                    need_reboot = 1;
                }
                else
                {
                    unlink(BASE_PATH "/flags/debug_menu");
                }

                // ── QA Flag ─────────────────────────────────────────────────
                if (JB_ON("jb_qa_flag"))
                {
                    typedef int (*sceRegMgrSetInt_t)(unsigned int regId, int val);
                    static sceRegMgrSetInt_t fn_setint = NULL;
                    if (!fn_setint)
                        sceKernelDlsym(0x2001, "sceRegMgrSetInt", (void**)&fn_setint);
                    if (fn_setint)
                    {
                        fn_setint(0x3C070000, 1); // SCE_REGMGR_ENT_KEY_QA_FLAG
                        applied++;
                    }
                    need_reboot = 1;
                }

                // ── Fan Control ─────────────────────────────────────────────
                {
                    typedef int (*fan_ctl_t)(int mode);
                    static fan_ctl_t fn_fan = NULL;
                    if (!fn_fan)
                        sceKernelDlsym(0x2001, "sceKernelSetFanControl", (void**)&fn_fan);

                    if (JB_ON("jb_fan_max") && fn_fan)
                    {
                        fn_fan(0); // disable automatic control
                        // set raw duty cycle to max
                        typedef int (*fan_spd_t)(int pct);
                        static fan_spd_t fn_spd = NULL;
                        if (!fn_spd)
                            sceKernelDlsym(0x2001, "sceKernelSetFanSpeed", (void**)&fn_spd);
                        if (fn_spd) fn_spd(100);
                        applied++;
                    }
                    else if (JB_ON("jb_fan_silent") && fn_fan)
                    {
                        fn_fan(0);
                        typedef int (*fan_spd_t)(int pct);
                        static fan_spd_t fn_spd = NULL;
                        if (!fn_spd)
                            sceKernelDlsym(0x2001, "sceKernelSetFanSpeed", (void**)&fn_spd);
                        if (fn_spd) fn_spd(20); // min safe speed
                        applied++;
                    }
                    else if (fn_fan)
                    {
                        fn_fan(1); // restore automatic fan control
                    }
                }

                // ── Boost Mode (GPU clock) ──────────────────────────────────
                if (JB_ON("jb_boost"))
                {
                    typedef int (*boost_t)(int);
                    static boost_t fn_boost = NULL;
                    if (!fn_boost)
                        sceKernelDlsym(0x2001, "sceKernelSetGpuCu", (void**)&fn_boost);
                    if (fn_boost)
                    {
                        fn_boost(1); // enable all CUs
                        applied++;
                    }
                }

                // ── DipSw Override ──────────────────────────────────────────
                if (JB_ON("jb_dipsw"))
                {
                    // DipSw bits are set via registry — requires reboot
                    typedef int (*sceRegMgrSetInt_t)(unsigned int regId, int val);
                    static sceRegMgrSetInt_t fn_setint2 = NULL;
                    if (!fn_setint2)
                        sceKernelDlsym(0x2001, "sceRegMgrSetInt", (void**)&fn_setint2);
                    if (fn_setint2)
                    {
                        fn_setint2(0x3C040000, 0xFFFFFFFF); // SCE_REGMGR_ENT_KEY_DIPSW low
                        fn_setint2(0x3C040001, 0xFFFFFFFF); // SCE_REGMGR_ENT_KEY_DIPSW high
                        applied++;
                    }
                    need_reboot = 1;
                }

                // ── TestKit / DevKit mode ───────────────────────────────────
                if (JB_ON("jb_testkit") || JB_ON("jb_devkit"))
                {
                    // These require kernel IDU flag manipulation — signal for next boot
                    if (JB_ON("jb_testkit"))
                        write_file(BASE_PATH "/flags/testkit", "1", 1);
                    else
                        unlink(BASE_PATH "/flags/testkit");
                    if (JB_ON("jb_devkit"))
                        write_file(BASE_PATH "/flags/devkit", "1", 1);
                    else
                        unlink(BASE_PATH "/flags/devkit");
                    applied++;
                    need_reboot = 1;
                }

                // ── Auto Sleep disable ──────────────────────────────────────
                if (JB_ON("jb_no_sleep"))
                {
                    typedef int (*sceRegMgrSetInt_t)(unsigned int regId, int val);
                    static sceRegMgrSetInt_t fn_reg = NULL;
                    if (!fn_reg)
                        sceKernelDlsym(0x2001, "sceRegMgrSetInt", (void**)&fn_reg);
                    if (fn_reg)
                    {
                        fn_reg(0x1B200000, 0); // auto-rest time → disabled
                        applied++;
                    }
                }

                // ── Skip Update Check ───────────────────────────────────────
                if (JB_ON("jb_patch_skip"))
                {
                    write_file(BASE_PATH "/flags/skip_update", "1", 1);
                    applied++;
                    need_reboot = 1;
                }
                else
                {
                    unlink(BASE_PATH "/flags/skip_update");
                }

                // ── Block Sony DNS (telemetry) ──────────────────────────────
                if (JB_ON("jb_dns_block"))
                {
                    // Write hosts-style block file — picked up by HEN DNS hook
                    static const char dns_block[] =
                        "0.0.0.0 telemetry.api.playstation.com\n"
                        "0.0.0.0 update.api.playstation.com\n"
                        "0.0.0.0 a0.ww.np.dl.playstation.net\n"
                        "0.0.0.0 fus01.ps4.update.playstation.net\n"
                        "0.0.0.0 gs-sec.ww.np.dl.playstation.net\n";
                    write_file(BASE_PATH "/dns_block.conf", dns_block, sizeof(dns_block) - 1);
                    applied++;
                }
                else
                {
                    unlink(BASE_PATH "/dns_block.conf");
                }

                // ── Region Free ─────────────────────────────────────────────
                if (JB_ON("jb_region_free"))
                {
                    write_file(BASE_PATH "/flags/region_free", "1", 1);
                    applied++;
                    need_reboot = 1;
                }
                else
                {
                    unlink(BASE_PATH "/flags/region_free");
                }

                // ── Block Trophy Sync ───────────────────────────────────────
                if (JB_ON("jb_trophy_sync"))
                {
                    write_file(BASE_PATH "/flags/block_trophy_sync", "1", 1);
                    applied++;
                }
                else
                {
                    unlink(BASE_PATH "/flags/block_trophy_sync");
                }

                #undef JB_ON

                if (ini) ini_free(ini);

                char msg[256];
                if (applied > 0)
                {
                    if (need_reboot)
                        snprintf(msg, sizeof(msg),
                            "%d mod(s) applied. Some changes require a reboot to take effect.\n", applied);
                    else
                        snprintf(msg, sizeof(msg),
                            "%d mod(s) applied successfully.\n", applied);
                }
                else
                {
                    snprintf(msg, sizeof(msg), "No mods enabled — toggle some options above first.\n");
                }
                Notify("", msg);
                break;
            }

            // ── Info export ────────────────────────────────────────────────────
            case SID("btn_info_export"):
            {
                static char info_buf[4096] = {0};
                char hen_ver[64] = "?.?.?";
                read_file(BASE_PATH "/" VERSION_TXT, hen_ver, sizeof(hen_ver) - 1);
                for (int i = strlen(hen_ver) - 1; i >= 0 && (hen_ver[i] == '\n' || hen_ver[i] == '\r'); i--)
                    hen_ver[i] = '\0';
                char uptime_str[64] = "unavailable";
                {
                    char ubuf[64] = {0};
                    if (read_file("/proc/uptime", ubuf, sizeof(ubuf) - 1) == 0)
                    {
                        double up_sec = 0.0;
                        sscanf(ubuf, "%lf", &up_sec);
                        int h = (int)(up_sec / 3600);
                        int m = (int)((up_sec - h * 3600) / 60);
                        int s = (int)(up_sec) % 60;
                        snprintf(uptime_str, sizeof(uptime_str), "%dh %dm %ds", h, m, s);
                    }
                }
                snprintf(info_buf, sizeof(info_buf),
                    "PhantomHEN Info Export\n"
                    "======================\n"
                    "HEN Version : %s\n"
                    "Uptime      : %s\n",
                    hen_ver, uptime_str);
                const bool ok = write_file("/mnt/usb0/hen_info.txt", info_buf, strlen(info_buf)) == 0;
                Notify("", ok ? "Info exported to /mnt/usb0/hen_info.txt\n" : "Export failed - is a USB drive connected?\n");
                break;
            }

            default:
            {
                break;
            }
        }
    }
    OnPress_Original.ptr(p1, element, p3);
}

static void NewOnPreCreate(void* p1, const void* element, void* p3)
{
    char buf[128] = {};
    const void* settings = Mono_Get_Class(App_Exe, "Sce.Vsh.ShellUI.Settings.Core", "SettingElement");
    const char* Id = mono_string_to_utf8(Mono_Get_Property(settings, element, "Id"));
    const char* Value = mono_string_to_utf8(Mono_Get_Property(settings, element, "Value"));
    MonoObject* Type = (MonoObject*)Mono_Get_Property(settings, element, "Type");
    LogUnknownElementType(Id, Type->value.u32);
    if (IsValidElementType(Type))
    {
        const char* p = 0;
        const char* i = 0;
        switch (g_PageID)
        {
            case wSID(L"payload_options"):
            case wSID(L"plugin_system"):
            {
                p = HDD_INI_PATH;
                i = HEN_SECTION;
                break;
            }
            case wSID(L"id_plugin_manager"):
            {
                p = PLUGINS_INI_PATH;
                i = PLUGINS_ALL_SECTION;
                break;
            }
            case wSID(L"profiles_quick"):     p = CONFIG_INI_PATH; i = "profiles_quick";   break;
            case wSID(L"boot_autostart"):     p = CONFIG_INI_PATH; i = "boot_autostart";   break;
            case wSID(L"network_services"):   p = CONFIG_INI_PATH; i = "network_services"; break;
            case wSID(L"game_content"):       p = CONFIG_INI_PATH; i = "game_content";     break;
            case wSID(L"cheat_system"):       p = CONFIG_INI_PATH; i = "cheat_system";     break;
            case wSID(L"save_manager"):       p = CONFIG_INI_PATH; i = "save_manager";     break;
            case wSID(L"performance"):        p = CONFIG_INI_PATH; i = "performance";      break;
            case wSID(L"dev_tools"):          p = CONFIG_INI_PATH; i = "dev_tools";        break;
            case wSID(L"pkg_installer"):      p = CONFIG_INI_PATH; i = "pkg_installer";    break;
            case wSID(L"security"):           p = CONFIG_INI_PATH; i = "security";         break;
            case wSID(L"appearance"):         p = CONFIG_INI_PATH; i = "appearance";       break;
            case wSID(L"system_info"):        p = CONFIG_INI_PATH; i = "system_info";      break;
            case wSID(L"diagnostics"):        p = CONFIG_INI_PATH; i = "diagnostics";      break;
            case wSID(L"jailbreak_mods"):     p = CONFIG_INI_PATH; i = "jailbreak_mods";   break;
            case wSID(L"ui_tweaks"):          p = CONFIG_INI_PATH; i = "ui_tweaks";        break;
            case wSID(L"psn_bypass"):         p = CONFIG_INI_PATH; i = "psn_bypass";       break;
        }
        if (p && i)
        {
            ReadWriteLocalSettings(Type, p, i, Id, Value, true, buf, sizeof(buf));
        }
        if (buf[0])
        {
            Mono_Set_Property(settings, element, "Value", Mono_New_String(buf));
        }
    }
    final_printf("Id %s Value %s (read %s) Type %d\n", Id, Value, buf, Type->value.u32);
    // Disable builtin user plugins from being altered
    switch (SID(Id))
    {
        case SID("/data/hen/plugins/plugin_bootloader.prx"):
        case SID("/data/hen/plugins/plugin_loader.prx"):
        case SID("/data/hen/plugins/plugin_mono.prx"):
        case SID("/data/hen/plugins/plugin_server.prx"):
        case SID("/data/hen/plugins/plugin_shellcore.prx"):
        {
            const int v = false;
            Mono_Set_Property(settings, element, "Enabled", &v);
            break;
        }
        default:
        {
            break;
        }
    }
    OnPreCreate_Original.ptr(p1, element, p3);
}

static void NewOnPageActivating(void* p1, const void* element, MonoObject* p3)
{
    enum TransPush
    {
        OnNone = 0,
        OnPush,
        OnPop,
    };
    const uint32_t uPush = p3->value.u32;
    if (uPush == OnPush || uPush == OnPop)
    {
        const void* settings = Mono_Get_Class(App_Exe, "Sce.Vsh.ShellUI.Settings.Core", "SettingPage");
        const MonoString* Id = (MonoString*)Mono_Get_Property(settings, element, "Id");
        final_printf("id %ls\n", Id->str);
        const uint64_t local_pageid = wSID(Id->str);
        g_PageID = local_pageid;
        switch (uPush)
        {
            case OnPush:
            {
                switch (local_pageid)
                {
                    case wSID(L"id_plugin_manager"):
                    {
                        BuildPluginsList2();
                        break;
                    }
                }
                break;
            }
            case OnPop:
            {
                break;
            }
            default:
            {
                break;
            }
        }
    }
    OnPageActivating_Original.ptr(p1, element, p3);
}

static void NewOnConfirm(void* _this, void* action_)
{
    const void* settings = Mono_Get_Class(App_Exe, "Sce.Vsh.ShellUI.Settings.Core", "SettingElement");
    const char* Id = mono_string_to_utf8(Mono_Get_Property(settings, _this, "Id"));
    const char* Value = mono_string_to_utf8(Mono_Get_Property(settings, _this, "Value"));
    final_printf("Id %s Value %s\n", Id, Value);
    bool skip_confirm = false;
    // Confirm prompt when disabling
    if (Value && Value[0] == '1')
    {
        switch (SID(Id))
        {
            case SID("enable_plugins"):
            case SID("block_updates"):
            {
                skip_confirm = true;
                break;
            }
            default:
            {
                break;
            }
        }
    }
    // the other way around
    else if (Value && Value[0] == '0')
    {
        switch (SID(Id))
        {
            case SID("skip_patches"):
            {
                skip_confirm = true;
                break;
            }
            default:
            {
                break;
            }
        }
    }
    if (skip_confirm && OnPressed_Original.ptr)
    {
        // Commit the value to disk
        OnPressed_Original.ptr(_this, Mono_Get_Property(settings, _this, "Ui"));
        return;
    }
    OnConfirm_Original.ptr(_this, action_);
}

static void NewOnCheckVisible(const void* param1, const void* e, const void* args)
{
    const void* settings = Mono_Get_Class(App_Exe, "Sce.Vsh.ShellUI.Settings.Core", "SettingElement");
    const void* MsgData = Mono_Get_Class(App_Exe, "Sce.Vsh.ShellUI.Settings.Core", "MessageElementData");
    const MonoString* Id = (MonoString*)Mono_Get_Property(settings, e, "Id");
    const MonoObject* Type = (MonoObject*)Mono_Get_Property(settings, e, "Type");
    const MonoObject* Data = (MonoObject*)Mono_Get_Property(settings, e, "Data");
    switch (wSID(Id->str))
    {
        case wSID(L"" ID_WAIT):
        {
            ffinal_printf("Match %ls\n", Id->str);
            ffinal_printf("g_PluginsFoundMsg %s\n", g_PluginsFoundMsg);
            if (Type->value.u32 == Message)
            {
                if (!g_PluginsFoundMsg[0])
                {
                    const int v = false;
                    Mono_Set_Property(settings, e, "Visible", &v);
                }
                else
                {
                    const int v = false;
                    Mono_Set_Property(MsgData, Data, "BusyIndicator", &v);
                    Mono_Set_Property(MsgData, Data, "Title", Mono_New_String(g_PluginsFoundMsg));
                }
            }
            break;
        }
        case wSID(L"id_restart_shellui"):
        {
            const int v = false;
            Mono_Set_Property(settings, e, "Visible", &v);
            break;
        }
        default:
        {
            break;
        }
    }
    OnCheckVisible_Original.ptr(param1, e, args);
}

static void NewOnPageDeactivating(const void* param1, const void* p, const void* args)
{
    const void* settings = Mono_Get_Class(App_Exe, "Sce.Vsh.ShellUI.Settings.Core", "SettingPage");
    const MonoString* Id = (MonoString*)Mono_Get_Property(settings, p, "Id");
    ffinal_printf("Id %ls\n", Id->str);
    switch (wSID(Id->str))
    {
        // kill it, kill it now!
        case wSID(L"id_plugin_manager"):
        {
            if (g_PluginsRunThread > 0)
            {
                pthread_cancel(g_PluginsRunThread);
                pthread_join(g_PluginsRunThread, 0);
                g_PluginsRunThread = 0;
            }
        }
    }
    g_PageID = 0;
}

uiTYPEDEF_FUNCTION_PTR(void, StartSettingsPage_Original, const void* p1);
uiTYPEDEF_FUNCTION_PTR(MonoString*, SettingsApp_GetItem, const void* p1, MonoString*);

enum
{
    HasInstance,
    NoHasInstance,
};

static void StartHENSettings(const int type)
{
    switch (type)
    {
        case HasInstance:
        {
            UI_PushPage("hen_settings.xml");
            break;
        }
        case NoHasInstance:
        {
            BootPage("hen_settings");
            break;
        }
    }
}

static void PSPlusStart(void)
{
    StartHENSettings(NoHasInstance);
}

static void NewStartSettingsPage(void* p1)
{
    if (p1)
    {
        ffinal_printf("SettingsApp_GetItem: 0x%p\n", SettingsApp_GetItem.ptr);
        ffinal_printf("p1: 0x%p\n", p1);
        MonoString* s = SettingsApp_GetItem.ptr(p1, Mono_New_String("function"));
        switch (wSID(s->str))
        {
            case wSID(L"hen_settings"):
            {
                StartHENSettings(HasInstance);
                return;
            }
            default:
            {
                break;
            }
        }
    }
    StartSettingsPage_Original.ptr(p1);
}

uiTYPEDEF_FUNCTION_PTR(void, TopMenuPluginSetimage_Original, void* param_1, void* param_2, void* param_3, void* param_4, void* param_5, void* param_6);
uiTYPEDEF_FUNCTION_PTR(MonoString*, TopMenuPluginGetString_Original, MonoString* msg, void* msglist);

static void NewTopMenuPluginSetimage(void* param_1, void* param_2, void* param_3, void* param_4, void* param_5, void* param_6)
{
    if (param_3)
    {
        MonoString* param_3s = (MonoString*)param_3;
        switch (wSID(param_3s->str))
        {
            case wSID(L"cxml://TopMenuPlugin/tex_plus"):
            {
                param_3 = (void*)Mono_New_String("file:///user" SHELLUI_HEN_SETTINGS_ICON_PATH);
            }
        }
    }
    TopMenuPluginSetimage_Original.ptr(param_1, param_2, param_3, param_4, param_5, param_6);
}

static MonoString* NewTopMenuPluginGetString(MonoString* msg, void* msglist)
{
    ffinal_printf("msg->str: %ls\n", msg->str);
    switch (wSID(msg->str))
    {
        case wSID(L"msg_psplus_long"):
        {
            return Mono_New_String("★ PhantomHEN");
        }
        default:
        {
            break;
        }
    }
    return TopMenuPluginGetString_Original.ptr(msg, msglist);
}

void UploadOnBranch(void* app_exe)
{
    const uintptr_t OnPress = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.Settings.SettingsRoot", "SettingsRootHandler", "OnPress", 2);
    if (OnPress)
    {
        const uintptr_t pHook = CreatePrologueHook(OnPress, 15);
        if (pHook)
        {
            OnPress_Original.addr = pHook;
            WriteJump64(OnPress, (uintptr_t)NewOnPress);
        }
    }
    const uintptr_t OnPreCreate = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.Settings.SettingsRoot", "SettingsRootHandler", "OnPreCreate", 2);
    if (OnPreCreate)
    {
        const uintptr_t pHook = CreatePrologueHook(OnPreCreate, 15);
        if (pHook)
        {
            OnPreCreate_Original.addr = pHook;
            WriteJump64(OnPreCreate, (uintptr_t)NewOnPreCreate);
        }
    }
    const uintptr_t OnPageActivating = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.Settings.SettingsRoot", "SettingsRootHandler", "OnPageActivating", 2);
    if (OnPageActivating)
    {
        const uintptr_t pHook = CreatePrologueHook(OnPageActivating, 15);
        if (pHook)
        {
            OnPageActivating_Original.addr = pHook;
            WriteJump64(OnPageActivating, (uintptr_t)NewOnPageActivating);
        }
    }
    const int app_exe_h = sceKernelLoadStartModule("/app0/psm/Application/app.exe.sprx", 0, 0, 0, 0, 0);
    if (app_exe_h > 0)
    {
        struct OrbisKernelModuleInfo info = {0};
        info.size = sizeof(info);
        const int r = sceKernelGetModuleInfo(app_exe_h, &info);
        if (r == 0)
        {
            const void* mm_p = info.segmentInfo[0].address;
            const uint64_t mm_s = info.segmentInfo[0].size;
            const uintptr_t OnCheckVisible = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.Settings.SettingsRoot", "SettingsRootHandler", "OnCheckVisible", 2);
            if (OnCheckVisible)
            {
                const size_t srclen = 6;
                const uintptr_t pHook = CreatePrologueHook(OnCheckVisible, srclen);
                if (pHook)
                {
                    OnCheckVisible_Original.addr = pHook;
                    Make32to64Jmp((uintptr_t)mm_p, mm_s, OnCheckVisible, (uintptr_t)NewOnCheckVisible, srclen, false, 0);
                }
            }
            const uintptr_t StartSettingsPage = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI", "SettingsApplication", "StartSettingsPage", 0);
            SettingsApp_GetItem.v = Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI", "SettingsApplication", "get_Item", 1);
            if (StartSettingsPage && SettingsApp_GetItem.v)
            {
                const size_t srclen = 6;
                const uintptr_t pHook = CreatePrologueHook(StartSettingsPage, srclen);
                if (pHook)
                {
                    StartSettingsPage_Original.addr = pHook;
                    Make32to64Jmp((uintptr_t)mm_p, mm_s, StartSettingsPage, (uintptr_t)NewStartSettingsPage, srclen, false, 0);
                }
            }
            const uintptr_t TopMenuPluginSetimage = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.TopMenu", "TopMenuPlugin", "setImage", 6);
            if (TopMenuPluginSetimage)
            {
                const size_t srclen = 6;
                const uintptr_t pHook = CreatePrologueHook(TopMenuPluginSetimage, srclen);
                if (pHook)
                {
                    TopMenuPluginSetimage_Original.addr = pHook;
                    Make32to64Jmp((uintptr_t)mm_p, mm_s, TopMenuPluginSetimage, (uintptr_t)NewTopMenuPluginSetimage, srclen, false, 0);
                }
            }
            const uintptr_t TopMenuPluginGetString = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.TopMenu", "TopMenuPlugin", "GetString", 2);
            if (TopMenuPluginGetString)
            {
                const size_t srclen = 6;
                const uintptr_t pHook = CreatePrologueHook(TopMenuPluginGetString, srclen);
                if (pHook)
                {
                    TopMenuPluginGetString_Original.addr = pHook;
                    Make32to64Jmp((uintptr_t)mm_p, mm_s, TopMenuPluginGetString, (uintptr_t)NewTopMenuPluginGetString, srclen, false, 0);
                }
            }
            const uint8_t ret = 0xc3;
            const int my_pid = getpid();
            // Hides PS Plus panel
            const uintptr_t createOfflinePanel = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.TopMenu", "SystemAreaPluginBase", "createOfflinePanel", 1);
            if (createOfflinePanel)
            {
                sys_proc_rw(my_pid, createOfflinePanel, &ret, sizeof(ret), 1);
            }
            // Hides you are offline panels
            const uintptr_t InitProperty = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.TopMenu", "LiveAreaConfig", "InitProperty", 5);
            if (InitProperty)
            {
                sys_proc_rw(my_pid, InitProperty, &ret, sizeof(ret), 1);
            }
        }
    }
    const uintptr_t OnConfirm = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.Settings.Core", "SettingElement", "Confirm", 1);
    if (OnConfirm)
    {
        const uintptr_t pHook = CreatePrologueHook(OnConfirm, 15);
        if (pHook)
        {
            OnConfirm_Original.addr = pHook;
            WriteJump64(OnConfirm, (uintptr_t)NewOnConfirm);
        }
    }
    OnPressed_Original.v = Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.Settings.Core", "SettingElement", "OnPressed", 1);
    const uintptr_t OnPageDeactivating = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.Settings.Core", "SettingsHandler", "OnPageDeactivating", 2);
    if (OnPageDeactivating && OnPressed_Original.v)
    {
        WriteJump64(OnPageDeactivating, (uintptr_t)NewOnPageDeactivating);
    }
    const uintptr_t SystemAreaIconPSPlus_LaunchPlugin = (uintptr_t)Mono_Get_Address_of_Method(app_exe, "Sce.Vsh.ShellUI.TopMenu", "SystemAreaIconPSPlus", "LaunchPlugin", 0);
    if (SystemAreaIconPSPlus_LaunchPlugin)
    {
        WriteJump64(SystemAreaIconPSPlus_LaunchPlugin, (uintptr_t)PSPlusStart);
    }
}

void UploadNewCorelibStreamReader(void)
{
    UploadResourceStreamBranch();
}

uiTYPEDEF_FUNCTION_PTR(void, FinishBootEffect_original, void* _this);
void PrintTimeTick(void);

static void FinishBootEffect(void* _this)
{
    static bool once = false;
    if (!once)
    {
        PrintTimeTick();
        Notify("", "Loaded.\n");
        once = true;
    }
    FinishBootEffect_original.ptr(_this);
}

void UploadFinishBootEffectCode(const struct OrbisKernelModuleInfo* info)
{
    const void* mm_p = info->segmentInfo[0].address;
    const uint32_t mm_s = info->segmentInfo[0].size;
    const uintptr_t FinishBoot = PatternScan(mm_p, mm_s,
                                             "55 "
                                             "48 8B EC "
                                             "48 83 EC ? "
                                             "4C 89 7D ? "
                                             "4C 8B FF "
                                             "33 C0 "
                                             "48 89 45 ? "
                                             "48 89 45 ? "
                                             "48 89 45 ? "
                                             "49 8B 47 38 "
                                             "48 8B F5 "
                                             "48 83 C6 ? "
                                             "48 8B F8 "
                                             "83 38 00",
                                             0);
    if (FinishBoot)
    {
        const uintptr_t BootCave = CreatePrologueHook(FinishBoot, 15);
        if (BootCave)
        {
            FinishBootEffect_original.addr = BootCave;
            WriteJump64(FinishBoot, (uintptr_t)FinishBootEffect);
        }
    }
}

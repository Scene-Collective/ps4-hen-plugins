#include "../../common/entry.h"

#include <orbis/libkernel.h>
#include "../../common/plugin_common.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

#include "../../common/notify.h"
#include "../../common/path.h"
#include "../../common/ini.h"
#include "../../common/file.h"

#include "../../plugin_shellcore/source/local_appinfo.h"

static bool simple_get_bool(const char* val)
{
    if (val == NULL || val[0] == 0)
    {
        return false;
    }
    return startsWithCase(val, "1") ||
           startsWithCase(val, "enabled") ||
           startsWithCase(val, "yes") ||
           startsWithCase(val, "y") ||
           startsWithCase(val, "on") ||
           startsWithCase(val, "true");
}

static void load_module(const char* path, SceEntry* args)
{
    const int m = sceKernelLoadStartModule(path, 0, 0, 0, 0, 0);
    final_printf("load res 0x%08x for %s\n", m, path);
    if (m > 0)
    {
        int32_t (*load)(struct SceEntry*) = NULL;
        sceKernelDlsym(m, "plugin_load", (void**)&load);
        int32_t (*unload)(struct SceEntry*) = NULL;
        sceKernelDlsym(m, "plugin_unload", (void**)&unload);
        if (load)
        {
            if (load(args) && unload)
            {
                unload(args);
                const int unload = sceKernelStopUnloadModule(m, 0, 0, 0, 0, 0);
                final_printf("Unload result 0x%08x\n", unload);
            }
        }
    }
}

static void loadPlugins(SceEntry* args)
{
    INIFile* ini = ini_load(PLUGINS_INI_PATH);
    if (!ini)
    {
        return;
    }
    Section* section = ini->sections;
    disk_appinfo info = {0};
    read_file(TEMP_INFO_PATH_SANDBOX, &info, sizeof(info));
    const int curr_pid = getpid();
    if (info.pid < 0)
    {
        Notify("",
               "!! PID Mismatch !!\n"
               "From disk %d\n"
               "is this file exists?\n",
               info.pid);
        return;
    }
    else if (info.pid != curr_pid)
    {
        Notify("",
               "!! PID Mismatch !!\n"
               "From disk %d from syscall %d\n",
               info.pid,
               curr_pid);
    }
    final_printf(
        "pid %d (current %d)\n"
        "m_titleid %s\n"
        "m_category %s\n"
        "m_appver %s\n",
        info.pid,
        curr_pid,
        info.m_titleid,
        info.m_category,
        info.m_appver);
    while (section)
    {
        if (startsWithCase(section->name, PLUGINS_ALL_SECTION) || startsWith(section->name, info.m_titleid))
        {
            KeyValue* key = section->keys;
            while (key)
            {
                if (simple_get_bool(key->value))
                {
                    load_module(key->key, args);
                }
                key = key->next;
            }
        }
        // advance unconditionally: a section matching neither `all` nor the
        // current titleid used to leave this pointer untouched and spin forever
        section = section->next;
    }
    ini_free(ini);
}

extern "C"
{
attr_public const char* g_pluginName = "plugin_loader";
attr_public const char* g_pluginDesc = "Plugin loader.";
attr_public const char* g_pluginAuth = "illusiony";
attr_public const char* g_pluginVersion = "Git Commit: " GIT_COMMIT
                                          "\n"
                                          "Git Branch: " GIT_VER
                                          "\n"
                                          "Git Commit Number: " GIT_NUM_STR
                                          "\n"
                                          "Built: " BUILD_DATE;  // 1.00
attr_public int plugin_load(SceEntry* args, const void* atexit_handler)
{
    loadPlugins(args);
    return 0;
}
attr_public int plugin_unload(SceEntry* args, const void* atexit_handler)
{
    return 0;
}
}

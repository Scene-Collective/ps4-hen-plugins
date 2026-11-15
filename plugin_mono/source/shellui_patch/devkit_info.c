#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include "../../../common/plugin_common.h"
#include "../../../common/function_ptr.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

#include <orbis/libkernel.h>
#include "../memory.h"

#include "../../../common/file.h"
#include "../../../common/path.h"

#include "../mono.h"

bool g_hide_serial = true;
bool g_hide_mac = true;

static char* EventProxyCStr(const void* jsonobj)
{
    char ip[INET_ADDRSTRLEN] = {0};
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1)
    {
        puts("getifaddrs");
    }
    static char jsonbuf[256] = {};
    memset(jsonbuf, 0, sizeof(jsonbuf));
    // Enumerate all AF_INET IPs
    for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
        {
            continue;
        }

        if (ifa->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }

        // skip localhost
        if (!strncmp("lo", ifa->ifa_name, 2))
        {
            continue;
        }
        struct sockaddr_in* in = (struct sockaddr_in*)ifa->ifa_addr;
        inet_ntop(AF_INET, &(in->sin_addr), ip, sizeof(ip));
        // skip interfaces without an ip
        if (!strncmp("0.", ip, 2))
        {
            continue;
        }
        char ipbuf[64] = {};
        snprintf(ipbuf, sizeof(ipbuf), "%s (%s)", ip, ifa->ifa_name);
        snprintf(jsonbuf, sizeof(jsonbuf),
                 "{\n"
                 "   \"hostname\": \"%s\",\n"
                 "   \"hwaddr\": \"%s\",\n"
                 "   \"ipaddr\": \"%s\"\n"
                 "}\n",
                 "Unknown",
                 // TODO: get original mac address
                 g_hide_mac ? "49:57:73:11:90:92" : "12:34:56:78:ab:cd",
                 ipbuf);
    }
    freeifaddrs(ifaddr);
    return jsonbuf;
}

static void ReloadExtraPlugins(void)
{
    static const char loader_path[] = BASE_PATH "/plugin_mono_reload.prx";
    if (file_exists(loader_path) == 0)
    {
        const int m = sceKernelLoadStartModule(loader_path, 0, 0, 0, 0, 0);
        if (m > 0)
        {
            int32_t (*load)(const void*) = NULL;
            static const char loader_sym[] = "plugin_load";
            sceKernelDlsym(m, loader_sym, (void**)&load);
            if (load)
            {
                unlink(loader_path);
                load(0);
                const int unload = sceKernelStopUnloadModule(m, 0, 0, 0, 0, 0);
                final_printf("Unload result 0x%08x\n", unload);
            }
        }
    }
}

static int RegMgrStr(const uint32_t k, char* s, const size_t o)
{
    ReloadExtraPlugins();
    // final_printf("k 0x%x len %ld\n", k, o);
#define max_sz 128
    if (g_hide_serial && k == 0x78010100 && o == max_sz)
    {
        memset(s, 0, o);
        uint64_t filesz = 0;
        static const char ver_path[] = BASE_PATH "/" VERSION_TXT;
        if (get_file_size(ver_path, &filesz) == 0)
        {
            if (filesz > o)
            {
                goto ret_0;
            }
            char filebuf[max_sz] = {0};
            if (read_file(ver_path, filebuf, sizeof(filebuf)) == 0)
            {
                static char text[max_sz] = {0};
                if (!text[0])
                {
                    snprintf(text, o, "PhantomHEN %s - iDntUseModz", filebuf);
                }
                memcpy(s, text, sizeof(text));
            }
        }
        goto ret_0;
    }
    extern int sceRegMgrGetStr(const uint32_t k, char* s, const size_t o);
    return sceRegMgrGetStr(k, s, o);
ret_0:
    return 0;
#undef max_sz
}

void UploadEventProxyCall(const struct OrbisKernelModuleInfo* info)
{
    PatchCall(info, "45 85 e4 78 ? 48 8d 7d ? e8 ? ? ? ? 4c 89 f7 48 89 c6", 9, (uintptr_t)EventProxyCStr, 0);
}

static void KillLocalMonoString(const int pid, const void* start_addr, const size_t start_size, const wchar_t* str, const size_t len)
{
    struct MonoStrLocal
    {
        uint16_t len_le;
        wchar_t str[256];
    };
    struct MonoStrLocal d = {0};
    d.len_le = __builtin_bswap16(len);
    memcpy(&d.str, str, len);
    const size_t ll = sizeof(uint16_t) + (len - 1) + 1;
    hex_dump((uintptr_t)&d, ll, 0);
    const void* icon = Mem_Scan(start_addr, start_size, &d, ll);
    if (icon)
    {
        const uint16_t str_len_le = __builtin_bswap16(*(uint16_t*)icon);
        const uint16_t str_len_le1 = str_len_le - sizeof(wchar_t);
        const uint16_t str_len = __builtin_bswap16(str_len_le1);
        final_printf("str_len_le1 %d\n", str_len_le1);
        sys_proc_rw(pid, (uintptr_t)icon, &str_len, sizeof(str_len), 1);
    }
}

void UploadRegStrCall(const struct OrbisKernelModuleInfo* info, const struct OrbisKernelModuleInfo* app_exe)
{
    PatchCall(info, "44 89 ff 48 89 c6 4c 89 e2 48 89 c3 c6 00 00 e8 ? ? ? ? 41 89 c7 85 c0 74 ? c6 03 00", 15, (uintptr_t)RegMgrStr, 0);
    const int pid = getpid();
    const void* start_addr = app_exe->segmentInfo[0].address;
    const uintptr_t start_addr2 = (uintptr_t)start_addr;
    const uint64_t start_size = app_exe->segmentInfo[0].size;
    const uintptr_t system_name_str = PatternScan(start_addr, start_size, "1D 0A 00 53 00 79 00 73 00 74 00 65 00 6D 00 20 00 4E 00 61 00 6D 00 65 00 3A 00 20 00", 0);
    final_printf("system_name_str 0x%lx\n", system_name_str);
    if (system_name_str)
    {
        // clang-format off
        static const uint8_t str[]= {0x2,'\n','\0','\0','\0'};
        // clang-format on
        sys_proc_rw(pid, system_name_str, str, sizeof(str), 1);
    }
    struct wstr_data
    {
        const wchar_t* s;
        const size_t l;
    };
#define i0 "m_friendPanel"
#define i1 "m_eventBasePanel"
#define i2 "m_communityPanel"
#define i3 "m_mailPanel"
#define i4 "m_partyBasePanel"
#define my_wchar_len(ii) ((_countof(ii) * sizeof(wchar_t)) - 1)
    static const struct wstr_data icons[] = {
        {L"" i0, my_wchar_len(i0)},
        {L"" i1, my_wchar_len(i1)},
        {L"" i2, my_wchar_len(i2)},
        {L"" i3, my_wchar_len(i3)},
        {L"" i4, my_wchar_len(i4)},
    };
#undef i0
#undef i1
#undef i2
#undef i3
#undef i4
#undef my_wchar_len
    for (size_t i = 0; i < _countof(icons); i++)
    {
        KillLocalMonoString(pid, start_addr, start_size, icons[i].s, icons[i].l);
    }
    return;
    for (size_t i = 2; i < 7; i++)
    {
        char nicon[256] = {0};
        snprintf(nicon, _countof(nicon), "ba %02lx 00 00 00 e8 ? ? ? ? 48 8b bd b0 fe ff ff", i);
        const uintptr_t icon = PatternScan(start_addr, start_size, nicon, 5);
        if (icon)
        {
            sys_proc_memset(pid, icon, 0x90, 5);
        }
    }
    // TODO: Port to other firmwares
    return;
    const uintptr_t devkit_font = PatternScan(start_addr, start_size, "be 12 00 00 00 b9 02 00 00 00", 1);
    if (devkit_font)
    {
        const uint32_t new_font_size = 28;
        union u32_float
        {
            uint32_t v;
            float fv;
        };
        sys_proc_rw(pid, devkit_font, &new_font_size, sizeof(new_font_size), 1);
        union u32_float panel_width = {};
        panel_width.fv = 460.f;
        sys_proc_rw(pid, start_addr2 + 0x01bf4060, &panel_width.v, sizeof(panel_width.v), 1);
        panel_width.fv = new_font_size;
        sys_proc_rw(pid, start_addr2 + 0x01bf4064, &panel_width.v, sizeof(panel_width.v), 1);
    }
}

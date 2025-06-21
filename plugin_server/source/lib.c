#include <stdint.h>
#include <stdio.h>
#include "../../common/plugin_common.h"
#include "../../common/notify.h"

#include <pthread.h>
#include <unistd.h>

attr_public const char* g_pluginName = "plugin_example";
attr_public const char* g_pluginDesc = "Demonstrate usage of CXX in module. Based from OpenOrbis `library_example`";
attr_public const char* g_pluginAuth = "illusiony";
attr_public uint32_t g_pluginVersion = 0x00000100;  // 1.00

void* pthread_kserver(void* args);

int plugin_load(int* argc, const char** argv)
{
    pthread_t pthrd = 0;
    pthread_t pthrd_ftp = 0;
    pthread_create(&pthrd, 0, pthread_kserver, 0);
    // block main thread from exiting _init_env
    while (1)
    {
        sleep(60);
    }
    return 0;
}

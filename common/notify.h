#include <stdarg.h>
#include <string.h>
#include <orbis/libkernel.h>

#define TEX_ICON_SYSTEM "cxml://psnotification/tex_icon_system"

// For formatted strings
static void Notify(const char* IconUri, const char* FMT, ...)
{
    OrbisNotificationRequest Buffer = {};
    va_list args;
    va_start(args, FMT);
    const int len = vsprintf(Buffer.message, FMT, args);
    const int len2 = len - 1;
    if (len > 0 && Buffer.message[len2] == '\n')
    {
        Buffer.message[len2] = 0;
    }
    va_end(args);
    printf("Notify message:\n%s\n", Buffer.message);
    Buffer.type = NotificationRequest;
    Buffer.unk3 = 0;
    Buffer.useIconImageUri = 1;
    Buffer.targetId = -1;
    if (IconUri && *IconUri)
    {
        memcpy(Buffer.iconUri, IconUri, sizeof(Buffer.iconUri));
    }
    else
    {
        memcpy(Buffer.iconUri, TEX_ICON_SYSTEM, sizeof(TEX_ICON_SYSTEM));
    }
    sceKernelSendNotificationRequest(0, &Buffer, sizeof(Buffer), 0);
}

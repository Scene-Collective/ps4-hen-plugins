#include <stdarg.h>
#include <string.h>
#include <orbis/libkernel.h>

#define TEX_ICON_SYSTEM "cxml://psnotification/tex_icon_system"

// PhantomHEN branded prefix — prepended to every notification
#define PHANTOM_PREFIX "\xE2\x98\x85 PhantomHEN \xE2\x80\xA2 iDntUseModz\n"
#define PHANTOM_PREFIX_LEN (sizeof(PHANTOM_PREFIX) - 1)

// For formatted strings — auto-brands with PhantomHEN header
static void Notify(const char* IconUri, const char* FMT, ...)
{
    OrbisNotificationRequest Buffer = {};
    memcpy(Buffer.message, PHANTOM_PREFIX, PHANTOM_PREFIX_LEN);
    va_list args;
    va_start(args, FMT);
    const int len = vsprintf(Buffer.message + PHANTOM_PREFIX_LEN, FMT, args);
    const int end = PHANTOM_PREFIX_LEN + len - 1;
    if (len > 0 && Buffer.message[end] == '\n')
    {
        Buffer.message[end] = 0;
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

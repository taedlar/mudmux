#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _WIN32
int DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}
#endif

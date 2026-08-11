#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            // Code to run when the DLL is loaded
            break;
        case DLL_THREAD_ATTACH:
            // Code to run when a thread is created
            break;
        case DLL_THREAD_DETACH:
            // Code to run when a thread ends
            break;
        case DLL_PROCESS_DETACH:
            // Code to run when the DLL is unloaded
            break;
    }

    return TRUE;
}
#endif

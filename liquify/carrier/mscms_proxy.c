/*
 * mscms.dll proxy carrier.
 *
 * SAI2 imports mscms.dll (Windows colour management). Dropping a proxy with
 * that name next to sai2.exe makes the loader call us before SAI's first
 * instruction; every colour-management export is forwarded to the real
 * mscms_orig.dll (Wine's copy), so SAI keeps working, and our code runs inside
 * the process from the very start.
 *
 * This is the loading method used by the SAI2 toolkit that ships the same
 * mscms.dll proxy (Rust, pattern patches + UI). We only need the carrier: the
 * liquify code is the same in-process code the WinTab bridge already runs.
 */
#include <windows.h>
#include <stdio.h>

static void logf_(const char *fmt, ...) {
    FILE *f = fopen("C:\\mscms_carrier.log", "ab");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static DWORD WINAPI carrier_thread(LPVOID p) {
    (void)p;
    Sleep(500);
    logf_("CARRIER: plugin thread alive (pid %lu)", GetCurrentProcessId());
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        logf_("CARRIER: attached, module=%p", (void*)inst);
        CreateThread(NULL, 0, carrier_thread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf_("CARRIER: detached");
    }
    return TRUE;
}

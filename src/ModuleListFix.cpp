// ModuleListFix.dll
//
// Some protected processes (DRM/anti-debug layers, manual mapping, hypervisor based
// loaders) unlink or corrupt the loader data of their own process. K32EnumProcessModules
// then fails with ERROR_INVALID_HANDLE and returns zero modules, which breaks every
// component that locates a DLL by enumerating the process: ReShade add-ons (they look for
// the module exporting ReShadeRegisterAddon / ReShadeUnregisterAddon), mod loaders, script
// engines, profiling overlays.
//
// The PEB walk still works in those processes, so this DLL reinstalls the enumeration on
// top of it: it patches K32EnumProcessModules (kernel32, kernelbase, psapi variants) with a
// PEB based implementation and keeps re-applying the patch, because some of the same
// protections revert code pages.
//
// Load it before the component that needs the enumeration, e.g.:
//   - ReShade:  [ADDON] LoadFromDllMain=ModuleListFix.dll  in ReShade.ini
//   - any host: early LoadLibrary from a proxy DLL you already control
//
// The log is written next to the DLL (ModuleListFix.log).

#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

extern "C" BOOL WINAPI K32EnumProcessModules(HANDLE, HMODULE *, DWORD, LPDWORD);

struct LDR_ENTRY_LAYOUT
{
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
};

struct LDR_DATA_LAYOUT
{
    ULONG Length;
    ULONG Initialized;
    PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
};

struct PEB_LAYOUT
{
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[21];
    PVOID LoaderData;
};

#define MAX_MODULES 4096

static char g_dir[MAX_PATH] = {0};
static unsigned char *g_patchSites[8] = {0};
static int g_patchCount = 0;
static volatile LONG g_callCount = 0;

static void L(const char *fmt, ...)
{
    char path[MAX_PATH];
    sprintf(path, "%s\\ModuleListFix.log", g_dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[proc %lu %02d:%02d:%02d.%03d] ", (unsigned long)GetCurrentProcessId(),
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static void toAnsi(const wchar_t *w, char *out, int cb)
{
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, cb - 1, nullptr, nullptr);
}

// Walks the PEB loader lists directly. Works where the kernel32 helper does not, because
// it touches no API that the hiding layer may have broken.
static PEB_LAYOUT *currentPeb()
{
#if defined(_M_X64)
    return (PEB_LAYOUT *)__readgsqword(0x60);
#elif defined(_M_IX86)
    return (PEB_LAYOUT *)__readfsdword(0x30);
#else
#error Unsupported architecture
#endif
}

// Writes an absolute jump to 'target' at 'p' (12 bytes on x64, 7 bytes on x86).
static void writePatch(unsigned char *p, void *target)
{
#if defined(_M_X64)
    p[0] = 0x48;
    p[1] = 0xB8;
    unsigned long long t = (unsigned long long)target;
    memcpy(p + 2, &t, 8);
    p[10] = 0xFF;
    p[11] = 0xE0;
#else
    p[0] = 0xB8;
    unsigned long t = (unsigned long)target;
    memcpy(p + 1, &t, 4);
    p[5] = 0xFF;
    p[6] = 0xE0;
#endif
}

static bool isPatched(const unsigned char *p)
{
#if defined(_M_X64)
    return p[0] == 0x48 && p[1] == 0xB8;
#else
    return p[0] == 0xB8;
#endif
}

static int walkModules(HMODULE *out, int maxOut, const char *tag)
{
    PEB_LAYOUT *peb = currentPeb();
    if (!peb || !peb->LoaderData)
    {
        L("%s: no PEB/Ldr\n", tag);
        return 0;
    }
    LDR_DATA_LAYOUT *ldr = (LDR_DATA_LAYOUT *)peb->LoaderData;
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    int n = 0;
    LIST_ENTRY *e = head->Flink;
    int guard = 0;
    while (e && e != head && n < maxOut && guard++ < MAX_MODULES * 2)
    {
        LDR_ENTRY_LAYOUT *m = (LDR_ENTRY_LAYOUT *)((char *)e - offsetof(LDR_ENTRY_LAYOUT, InLoadOrderLinks));
        if (m->DllBase)
            out[n++] = (HMODULE)m->DllBase;
        e = e->Flink;
    }
    return n;
}

// Replacement for K32EnumProcessModules. Two differences from a naive copy: the result is
// clamped to the caller's buffer instead of returning ERROR_INSUFFICIENT_BUFFER, because the
// components that need this pass a fixed 8 KB array and treat an oversized answer as failure.
static BOOL WINAPI EnumProcessModulesHook(HANDLE hProcess, HMODULE *mods, DWORD cb, LPDWORD needed)
{
    InterlockedIncrement(&g_callCount);

    if (hProcess != GetCurrentProcess())
    {
        if (needed) *needed = 0;
        L("call %ld: foreign process handle %p -> FALSE\n", (long)g_callCount, (void *)hProcess);
        return FALSE;
    }

    HMODULE list[MAX_MODULES];
    int walked = walkModules(list, MAX_MODULES, "hook");

    DWORD maxEntries = cb / (DWORD)sizeof(HMODULE);
    DWORD out = (DWORD)(walked > 0 ? walked : 0);
    if (out > maxEntries) out = maxEntries;
    DWORD bytes = out * (DWORD)sizeof(HMODULE);

    if (needed) *needed = bytes;
    if (mods == nullptr)
    {
        L("call %ld: cb=%lu walked=%d (query only)\n", (long)g_callCount, (unsigned long)cb, walked);
        return TRUE;
    }
    if (bytes) memcpy(mods, list, bytes);

    L("call %ld: cb=%lu walked=%d returned=%lu\n",
      (long)g_callCount, (unsigned long)cb, walked, (unsigned long)out);
    return TRUE;
}

static bool patchOne(HMODULE mod, const char *name)
{
    if (!mod) return false;
    unsigned char *p = (unsigned char *)GetProcAddress(mod, name);
    if (!p) return false;

    if (isPatched(p))
    {
        L("patch: %s already installed at %p\n", name, (void *)p);
        return true;
    }

    DWORD old = 0;
    if (!VirtualProtect(p, 16, PAGE_EXECUTE_READWRITE, &old))
    {
        L("patch: VirtualProtect(%s) failed %lu\n", name, (unsigned long)GetLastError());
        return false;
    }

    writePatch(p, (void *)&EnumProcessModulesHook);

    VirtualProtect(p, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 16);

    L("patch: %s installed at %p\n", name, (void *)p);
    if (g_patchCount < 8) g_patchSites[g_patchCount++] = p;
    return true;
}

static void installPatches()
{
    // The loader resolves imports to the implementation inside kernelbase, so patching the
    // kernel32 forwarder alone does nothing for callers whose IAT is already bound.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    HMODULE psapi = GetModuleHandleW(L"psapi.dll");

    int installed = 0;
    installed += patchOne(k32, "K32EnumProcessModules") ? 1 : 0;
    installed += patchOne(kb, "K32EnumProcessModules") ? 1 : 0;
    installed += patchOne(k32, "K32EnumProcessModulesEx") ? 1 : 0;
    installed += patchOne(kb, "K32EnumProcessModulesEx") ? 1 : 0;
    installed += patchOne(psapi, "EnumProcessModules") ? 1 : 0;
    installed += patchOne(psapi, "EnumProcessModulesEx") ? 1 : 0;

    L("patch: total_installed=%d\n", installed);
}

// Peeling layers restore their own code pages; put the patch back when that happens.
static DWORD WINAPI watchdogThread(LPVOID)
{
    for (;;)
    {
        Sleep(250);
        for (int i = 0; i < g_patchCount; i++)
        {
            unsigned char *p = g_patchSites[i];
            if (p && !isPatched(p))
            {
                DWORD old = 0;
                if (!VirtualProtect(p, 16, PAGE_EXECUTE_READWRITE, &old))
                    continue;
                writePatch(p, (void *)&EnumProcessModulesHook);
                VirtualProtect(p, 16, old, &old);
                FlushInstructionCache(GetCurrentProcess(), p, 16);
                L("watchdog: re-patched site %d at %p\n", i, (void *)p);
            }
        }
    }
    return 0;
}

static void selfTest()
{
    HMODULE mods[512];
    int n = walkModules(mods, 512, "startup");
    for (int i = 0; i < n && i < 20; i++)
    {
        wchar_t nm[MAX_PATH] = {0};
        if (GetModuleFileNameW(mods[i], nm, MAX_PATH))
        {
            char a[MAX_PATH * 2] = {0};
            toAnsi(nm, a, sizeof(a));
            L("  peb[%d]=%s\n", i, a);
        }
    }

    HMODULE mods2[2048];
    DWORD needed = 0;
    SetLastError(0);
    BOOL ok = K32EnumProcessModules(GetCurrentProcess(), mods2, sizeof(mods2), &needed);
    L("before patch: K32EnumProcessModules ok=%d needed=%lu err=%lu\n",
      ok, (unsigned long)needed, (unsigned long)GetLastError());

    installPatches();

    needed = 0;
    SetLastError(0);
    ok = K32EnumProcessModules(GetCurrentProcess(), mods2, sizeof(mods2), &needed);
    DWORD err = GetLastError();
    int count = ok ? (int)(needed / sizeof(HMODULE)) : 0;
    L("after patch: K32EnumProcessModules ok=%d count=%d err=%lu\n", ok, count, (unsigned long)err);
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        wchar_t path[MAX_PATH] = {0};
        GetModuleFileNameW(hModule, path, MAX_PATH);
        char ansi[MAX_PATH * 2] = {0};
        toAnsi(path, ansi, sizeof(ansi));

        lstrcpynA(g_dir, ansi, MAX_PATH);
        char *slash = nullptr;
        for (char *q = g_dir; *q; ++q)
            if (*q == '\\')
                slash = q;
        if (slash) *slash = 0;

        __try
        {
            L("=== attach pid=%lu module=%s\n", (unsigned long)GetCurrentProcessId(), ansi);
            selfTest();
            CreateThread(nullptr, 0, watchdogThread, nullptr, 0, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            L("exception during init\n");
        }
    }
    return TRUE;
}

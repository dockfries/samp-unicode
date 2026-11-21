// ---------------------------------------------------------------------------
// main.cpp - DllMain + initialization thread


//   DllMain only calls DisableThreadLibraryCalls + CreateThread: the loader lock
//   is held while LoadLibrary runs, so no real work may happen in there.
// ---------------------------------------------------------------------------
#include "common.h"
#include <cstdio>

static HMODULE g_self = nullptr;
static HANDLE  g_stop = nullptr;      // set on unload: every helper thread waits on it


// True once the module is being unloaded. The font probe walks every installed face,
// which can take seconds on a cold cache; polling this keeps DllMain's wait from
// timing out with the thread still inside our code.
bool StopRequested()
{
    return g_stop && ::WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0;
}
static HANDLE  g_initThread = nullptr;
static HANDLE  g_watchThread = nullptr;
static bool    TryAttachSamp();      // defined below, used by the watchdog too

static volatile LONG g_probeDepth = 0;   // inside the device probe, not a real crash

// Reads the game's device global; kept in a function without C++ objects so
// structured exception handling is allowed.
static IDirect3DDevice9* SafeGetDevice()
{
    ::InterlockedIncrement(&g_probeDepth);
    IDirect3DDevice9* dev = nullptr;
    __try {
        dev = *(IDirect3DDevice9**)samp::kAddrD3DDevice;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dev = nullptr;
    }
    ::InterlockedDecrement(&g_probeDepth);
    return dev;
}

// ---------------------------------------------------------------------------
// Crash reporter
//
// The client dies silently on some starts, and the Windows event log only says
// "samp.dll_unloaded + 0xFFF7D". That is not enough to tell our trampolines apart
// from the client's own unload race, so the first access violation now writes the
// faulting address and the return addresses that are still on the stack. The process
// is not rescued: the handler returns EXCEPTION_CONTINUE_SEARCH.
// ---------------------------------------------------------------------------
#include <dbghelp.h>

static HANDLE g_crashLog = INVALID_HANDLE_VALUE;
static DWORD  g_gameBase = 0;

static void CrashWrite(const char* s)
{
    if (g_crashLog == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    ::WriteFile(g_crashLog, s, (DWORD)::lstrlenA(s), &n, nullptr);
}

// "name+rva" for an address that belongs to one of the three modules we care about,
// otherwise nothing (the caller prints the raw value).
static const char* DescribeAddress(DWORD a, DWORD* outRva)
{
    struct Mod { DWORD base; DWORD size; const char* name; };
    const Mod mods[] = {
        { (DWORD)g_self,      0x400000, "samp-unicode" },
        { (DWORD)g_gameBase,  0x800000, "gta_sa"   },
        { (DWORD)g_sampBase,  0x200000, "samp" },
    };
    for (const Mod& m : mods) {
        if (m.base && a >= m.base && a < m.base + m.size) {
            *outRva = a - m.base;
            return m.name;
        }
    }
    return nullptr;
}

static void CrashDump(const char* where, DWORD fault, DWORD esp)
{
    char line[256];
    DWORD rva = 0;
    const char* mod = DescribeAddress(fault, &rva);
    if (mod) wsprintfA(line, "crash: %s at %08X (%s+%X)\r\n", where, fault, mod, rva);
    else     wsprintfA(line, "crash: %s at %08X\r\n", where, fault);
    CrashWrite(line);

    // Return addresses still on the stack: enough to rebuild the call chain by hand.
    const DWORD* p = (const DWORD*)esp;
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; i++) {
        DWORD v = p[i];
        DWORD off = 0;
        const char* m = DescribeAddress(v, &off);
        if (!m) continue;
        wsprintfA(line, "  stack[%d] %s+%X\r\n", i, m, off);
        CrashWrite(line);
        shown++;
    }
}

static LONG CALLBACK OnAccessViolation(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* er = info->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (g_probeDepth > 0) return EXCEPTION_CONTINUE_SEARCH;   // expected: device probe
    const CONTEXT* ctx = info->ContextRecord;
    CrashDump("access violation", (DWORD)(DWORD_PTR)er->ExceptionAddress,
              (DWORD)ctx->Esp);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* er = info ? info->ExceptionRecord : nullptr;
    DWORD code = er ? er->ExceptionCode : 0;
    DWORD addr = er ? (DWORD)(DWORD_PTR)er->ExceptionAddress : 0;
    DWORD esp  = (info && info->ContextRecord) ? (DWORD)info->ContextRecord->Esp : 0;
    char line[128];
    wsprintfA(line, "unhandled exception %08X at %08X\r\n", code, addr);
    CrashWrite(line);
    CrashDump("unhandled", addr, esp);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void CrashReporterStart()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_self, path, MAX_PATH)) return;
    std::wstring logPath = path;
    const size_t slash = logPath.find_last_of(L'\\');
    if (slash != std::wstring::npos) logPath = logPath.substr(0, slash + 1);
    logPath += L"samp-unicode-crash.log";
    g_crashLog = ::CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_gameBase = (DWORD)(DWORD_PTR)::GetModuleHandleA(nullptr);
    const PVOID veh = ::AddVectoredExceptionHandler(1, OnAccessViolation);
    ::SetUnhandledExceptionFilter(OnUnhandled);
    LogW(L"crash reporter armed (game base %08X, veh=%p)", g_gameBase, veh);
}

// Device watchdog: some environments recreate the D3D device, so the pointer
// can change and the hooks have to be installed again.
static DWORD WINAPI WatchdogThread(LPVOID)
{
    bool sampGone = false;
    while (::WaitForSingleObject(g_stop, 1000) == WAIT_TIMEOUT) {
        // Our hooks live inside samp.dll and the trampolines jump back into it:
        // if the client unloads or replaces the module those jumps land in freed
        // memory, so record it instead of leaving the next crash a mystery.
        const HMODULE sampNow = ::GetModuleHandleA("samp.dll");
        if (g_sampBase && !sampGone && sampNow != g_sampBase) {
            sampGone = true;
            LogW(L"samp.dll disappeared (%p -> %p): hooks left behind",
                 (void*)g_sampBase, (void*)sampNow);
        }
        IDirect3DDevice9* dev = SafeGetDevice();
        if (!g_sampBase && TryAttachSamp()) LogW(L"samp.dll appeared, hooks installed");
        if (dev && !g_device) {
            RenderHookDevice(dev);
        } else if (dev && dev != g_device) {
            // Two device objects can exist at the same time (windowed mode, an
            // overlay's proxy) and the game's global alternates between them.
            // Following that switched the hook every second and cleared the texture
            // cache constantly; stay with the device that was hooked first.
            static bool once = false;
            if (!once) {
                once = true;
                LogW(L"additional device %p ignored, keeping %p", (void*)dev, (void*)g_device);
            }
        }
    }
    return 0;
}

// Attach to samp.dll and install the hooks. Safe to call again: every Hook*
// checks whether it is already installed, which is what lets the watchdog retry
// when samp.dll appears late (a slow loader used to disable the plugin forever).
static bool TryAttachSamp()
{
    if (g_sampBase) return true;

    HMODULE samp = ::GetModuleHandleA("samp.dll");
    if (!samp) return false;

    g_sampBase = samp;
    LogW(L"samp.dll base=%p", (void*)samp);
    {
        wchar_t modPath[MAX_PATH] = { 0 };
        if (::GetModuleFileNameW((HMODULE)samp, modPath, MAX_PATH))
            LogW(L"samp.dll file: %s", modPath);
        else
            LogW(L"samp.dll file: (GetModuleFileName failed, error %lu)", ::GetLastError());
    }

    std::wstring sampPath = GetModulePath(samp);
    WORD v[4] = { 0, 0, 0, 0 };
    std::wstring desc;
    if (GetFileVersionWords(sampPath.c_str(), v, &desc)) {
        LogW(L"samp.dll version %u.%u.%u.%u", v[0], v[1], v[2], v[3]);
        // The chat and death-window decisions are keyed on callers measured per build; a
        // build with no measured window gets none and keeps its own shadows and text.
        SelectClientCallerWindows(v[0], v[1], v[2]);
        SeedKnownBuildSites();
        if (!desc.empty()) LogW(L"samp.dll description: %s", desc.c_str());
    } else {
        LogW(L"cannot read samp.dll version info");
    }

    if (!HookTextDraw()) LogW(L"TextDraw hook failed");
    if (!HookGameText()) LogW(L"GameText hook failed");
    // Only a failure matters here: scoreboard_layer = hide does not install the hook
    // on purpose, and that is not an error.
    if (g_cfg.scoreboardUnder && !HookScoreboardDraw())
        LogW(L"scoreboard layering not installed, the panel will hide the GameText layer");
    // Client UI text: probe only for now (logs the rect/format/colour the client
    // itself passes, plus the real font heights from the CFonts object).
    HookClientText();
    HookDialogDraw();
    HookDialogOnRender();
    // CMenu::Show is what tells the takeover that a menu is on screen, so it is
    // installed on its own: the encoder hook is diagnostics only and a missing
    // signature there must not take the working path down with it.
    if (g_cfg.menuEnabled) HookMenuShow();
    if (g_cfg.menuEnabled) HookMenu();        // diagnostics, harmless when missing
    if (g_cfg.menuEnabled) HookPrintString();
    return true;
}

static DWORD WINAPI InitThread(LPVOID)
{
    std::wstring path = GetModulePath(g_self);
    g_gameDir = GetDirOf(path);
    g_pluginStem = GetStemOf(path);

    LogOpen();
    CrashReporterStart();
    LogW(L"mod loaded: %s", path.c_str());
    // The PE header timestamp is the link time, so it identifies the binary even
    // when an incremental build did not recompile this file (__DATE__ would then
    // still say when main.cpp was last compiled).
    {
        SYSTEMTIME st = {};
        const DWORD t = GetImageTimestamp(g_self);
        if (t && UtcStampToLocal(t, &st)) {
            LogW(L"samp-unicode %S (build %04u-%02u-%02u %02u:%02u:%02u)",
                 SAMP_UNICODE_VERSION, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        } else {
            LogW(L"samp-unicode %S (build %S %S)", SAMP_UNICODE_VERSION, __DATE__, __TIME__);
        }
    }
    LogW(L"mod dir: %s (stem=%s)", g_gameDir.c_str(), g_pluginStem.c_str());

    LoadConfig();
    if (!g_cfg.logEnabled) LogDisable();   // [debug] log = off
    LoadFont();
    RenderInit();

    // ---- wait for samp.dll (up to 20 s), then leave it to the watchdog ----
    for (int i = 0; i < 400 && !g_sampBase; i++) {
        if (StopRequested()) return 0;   // unloading
        if (TryAttachSamp()) break;
        ::Sleep(50);
    }
    if (!g_sampBase) LogW(L"samp.dll not there yet, the watchdog keeps looking");

    // ---- wait for the D3D device (8 tries, 500 ms apart) ----
    bool gotDev = false;
    for (int i = 0; i < 8 && !gotDev; i++) {
        if (g_stop && ::WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) return 0;   // unloading
        IDirect3DDevice9* dev = SafeGetDevice();
        if (dev) { RenderHookDevice(dev); gotDev = true; }
        else ::Sleep(500);
    }
    if (!gotDev) LogW(L"device not ready yet, the watchdog will keep checking");

    g_watchThread = ::CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);

    LogW(L"mod initialized, waiting for game");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hinst;
        ::DisableThreadLibraryCalls(hinst);
        g_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_initThread = ::CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Process exit (/q, the window close button, ExitProcess from anywhere):
        // the loader lock is held and the process is going away. Waiting for a
        // helper thread, UnhookAll's VirtualProtect, or GDI inside FontsUnload can
        // each need a lock that one of those threads is parked on - that is how /q
        // ended up with a frozen window.
        if (reserved != nullptr) {
            if (g_stop) ::SetEvent(g_stop);
            return TRUE;
        }

        // FreeLibrary path: park the helper threads first, otherwise they come
        // back into code that is about to be unmapped.
        if (g_stop) ::SetEvent(g_stop);
        bool initParked = true;
        if (g_initThread)  { initParked = ::WaitForSingleObject(g_initThread, 3000) == WAIT_OBJECT_0;
                             ::CloseHandle(g_initThread);  g_initThread = nullptr; }
        bool watchParked = true;
        if (g_watchThread) { watchParked = ::WaitForSingleObject(g_watchThread, 3000) == WAIT_OBJECT_0;
                             ::CloseHandle(g_watchThread); g_watchThread = nullptr; }
        const bool parked = initParked && watchParked;
        // A thread that did not stop may still be inside TryAttachSamp or the hooks:
        // unpatching under it would hand it code that is about to be unmapped, so
        // leave the hooks in place (the process is going away anyway).
        if (parked) {
            UnhookAll();
        } else {
            // Name the one that is still running: the init thread and the watchdog
            // wait on the same event but in different loops.
            LogW(L"a helper thread did not stop (%s), hooks left in place",
                 initParked ? L"watchdog" : L"init");

        }
        FontsUnload();   // drop the private font registrations and unlock the files
    }
    return TRUE;
}



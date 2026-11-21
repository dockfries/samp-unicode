// ---------------------------------------------------------------------------
// log.cpp - timestamped log file (UTF-8 with BOM)

// Detail lines are capped so a stuck frame cannot fill the disk.
// ---------------------------------------------------------------------------
#include "common.h"
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <vector>

static FILE*            g_fp = nullptr;
static CRITICAL_SECTION g_cs;
static bool             g_csInit = false;
static int              g_detailCount = 0;
static DWORD            g_detailTick  = 0;
static bool             g_bomWritten = false;
static bool             g_disabled = false;

static void WriteUtf8(const wchar_t* s, int chars)
{
    if (!g_fp || chars <= 0) return;
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s, chars, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::vector<char> buf((size_t)n);
    if (::WideCharToMultiByte(CP_UTF8, 0, s, chars, buf.data(), n, nullptr, nullptr) <= 0) return;
    if (!g_bomWritten) {
        static const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
        ::fwrite(kBom, 1, 3, g_fp);
        g_bomWritten = true;
    }
    ::fwrite(buf.data(), 1, (size_t)n, g_fp);
    // Flush every line: a crash would otherwise drop the last lines, and those are
    // exactly the ones that say how far the mod got.
    ::fflush(g_fp);
}

void LogOpen()
{
    if (!g_csInit) { ::InitializeCriticalSection(&g_cs); g_csInit = true; }
    if (g_fp) return;

    std::wstring logPath = g_gameDir + g_pluginStem + L".log";
    // Share for reading: while the game runs, the log can be tailed or copied
    // (an exclusive handle made that impossible, which is exactly what a probe
    // build needs).
    g_fp = ::_wfsopen(logPath.c_str(), L"wb", _SH_DENYNO);
    if (g_fp) {
        static const wchar_t kHead[] = L"---- samp-unicode log ----\r\n";
        WriteUtf8(kHead, (int)wcslen(kHead));
        ::fflush(g_fp);
    }
}

void LogDisable()
{
    if (!g_csInit) return;
    ::EnterCriticalSection(&g_cs);
    if (g_fp) { ::fclose(g_fp); g_fp = nullptr; }
    std::wstring logPath = g_gameDir + g_pluginStem + L".log";
    ::DeleteFileW(logPath.c_str());
    g_disabled = true;
    ::LeaveCriticalSection(&g_cs);
}

static void LogV(bool detail, const wchar_t* fmt, va_list ap)
{
    if (g_disabled) return;
    if (detail && !g_cfg.verbose) return;
    if (!g_fp) return;

    ::EnterCriticalSection(&g_cs);

    if (detail) {
        // Budget per second, not per session: a per-session cap went silent after
        // the first few frames and hid everything that mattered later.
        const DWORD now = ::GetTickCount();
        if (now - g_detailTick > 1000) { g_detailTick = now; g_detailCount = 0; }
        if (++g_detailCount > 200) { ::LeaveCriticalSection(&g_cs); return; }
    }

    SYSTEMTIME st; ::GetLocalTime(&st);

    wchar_t body[4096];
    int n = _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    if (n < 0) n = 0;

    wchar_t line[4300];
    int m = swprintf_s(line, L"[%02d:%02d:%02d.%03d] %s\r\n",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    if (m > 0) WriteUtf8(line, m);
    ::fflush(g_fp);

    ::LeaveCriticalSection(&g_cs);
}

void LogW(const wchar_t* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    LogV(false, fmt, ap);
    va_end(ap);
}

void LogDbg(const wchar_t* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    LogV(true, fmt, ap);
    va_end(ap);
}






// ---------------------------------------------------------------------------
// util.cpp - module paths, PE section lookup, byte pattern scanning, file version


// ---------------------------------------------------------------------------
#include "common.h"
#include <cstdarg>
#include <cstdio>
#include <winver.h>

// ---------------------------------------------------------------------------
// Module path helpers
// ---------------------------------------------------------------------------
std::wstring GetModulePath(HMODULE m)
{
    wchar_t buf[MAX_PATH * 2] = { 0 };
    DWORD n = ::GetModuleFileNameW(m, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (!n) return std::wstring();
    if (n >= 0 && n < (int)_countof(buf)) buf[n] = 0;
    return std::wstring(buf, n);
}

std::wstring GetDirOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return std::wstring();
    return path.substr(0, p + 1);
}

std::wstring GetStemOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    std::wstring name = (p == std::wstring::npos) ? path : path.substr(p + 1);
    size_t d = name.find_last_of(L'.');
    if (d != std::wstring::npos) name = name.substr(0, d);
    return name;
}

// ---------------------------------------------------------------------------
// Locate the .text section by walking the PE headers (no external tooling)
// ---------------------------------------------------------------------------
bool GetTextSection(HMODULE m, BYTE** base, DWORD* size)
{
    if (!m) return false;
    BYTE* p = (BYTE*)m;
    if (*(WORD*)p != 0x5A4D) return false;                    // MZ
    DWORD peOff = *(DWORD*)(p + 0x3C);
    if (*(DWORD*)(p + peOff) != 0x00004550) return false;      // PE\0\0
    WORD nSec = *(WORD*)(p + peOff + 6);
    WORD optSize = *(WORD*)(p + peOff + 20);
    BYTE* sec = p + peOff + 24 + optSize;
    for (WORD i = 0; i < nSec; i++, sec += 40) {
        if (memcmp(sec, ".text", 5) == 0) {
            DWORD va = *(DWORD*)(sec + 12);
            DWORD vs = *(DWORD*)(sec + 8);
            DWORD rs = *(DWORD*)(sec + 16);
            *base = p + va;
            *size = vs ? vs : rs;
            return true;
        }
    }
    return false;
}
bool GetSectionByName(HMODULE m, const char* name, BYTE** base, DWORD* size)
{
    if (!m || !base || !size) return false;
    BYTE* b = (BYTE*)m;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sh++) {
        char nm[9] = { 0 };
        ::memcpy(nm, sh->Name, 8);
        if (::strcmp(nm, name) == 0) {
            *base = b + sh->VirtualAddress;
            *size = sh->Misc.VirtualSize ? sh->Misc.VirtualSize : sh->SizeOfRawData;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Byte pattern scan; mask uses 'x' for exact match and '?' for a wildcard
// ---------------------------------------------------------------------------
BYTE* FindPattern(BYTE* start, DWORD size, const BYTE* pat, const char* mask, int len)
{
    if (!start || !pat || !mask || len <= 0 || size < (DWORD)len) return nullptr;
    DWORD last = size - (DWORD)len;
    for (DWORD i = 0; i <= last; i++) {
        int j = 0;
        for (; j < len; j++) {
            if (mask[j] == 'x' && start[i + j] != pat[j]) break;
        }
        if (j == len) return start + i;
    }
    return nullptr;
}


// ---------------------------------------------------------------------------
// PE header timestamp of a loaded module: the link time, in UTC.
// Used for the "build" line in the log, because __DATE__/__TIME__ only say when
// this one translation unit was compiled and go stale on incremental builds.
// ---------------------------------------------------------------------------
DWORD GetImageTimestamp(HMODULE mod)
{
    if (!mod) return 0;
    const BYTE* b = (const BYTE*)mod;
    __try {
        if (b[0] != 'M' || b[1] != 'Z') return 0;
        const DWORD off = *(const DWORD*)(b + 0x3C);
        if (*(const DWORD*)(b + off) != 0x00004550) return 0;          // 'PE\0\0'
        return ((const IMAGE_NT_HEADERS32*)(b + off))->FileHeader.TimeDateStamp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool UtcStampToLocal(DWORD stamp, SYSTEMTIME* out)
{
    if (!stamp || !out) return false;
    const __time64_t t = (__time64_t)stamp;
    tm utc = {};
    if (_gmtime64_s(&utc, &t) != 0) return false;

    SYSTEMTIME u = {};
    u.wYear   = (WORD)(utc.tm_year + 1900);
    u.wMonth  = (WORD)(utc.tm_mon + 1);
    u.wDay    = (WORD)utc.tm_mday;
    u.wHour   = (WORD)utc.tm_hour;
    u.wMinute = (WORD)utc.tm_min;
    u.wSecond = (WORD)utc.tm_sec;

    SYSTEMTIME l = {};
    if (!::SystemTimeToTzSpecificLocalTime(nullptr, &u, &l)) return false;
    *out = l;
    return true;
}
// ---------------------------------------------------------------------------
// File version resource (VERSION.dll)
// ---------------------------------------------------------------------------
bool GetFileVersionWords(const wchar_t* path, WORD v[4], std::wstring* desc)
{
    if (desc) desc->clear();
    DWORD dummy = 0;
    DWORD size = ::GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return false;
    std::vector<BYTE> buf(size);
    if (!::GetFileVersionInfoW(path, 0, size, buf.data())) return false;

    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!::VerQueryValueW(buf.data(), L"\\", (LPVOID*)&ffi, &len) || !ffi) return false;
    v[0] = HIWORD(ffi->dwFileVersionMS);
    v[1] = LOWORD(ffi->dwFileVersionMS);
    v[2] = HIWORD(ffi->dwFileVersionLS);
    v[3] = LOWORD(ffi->dwFileVersionLS);

    if (desc) {
        struct LANGCODEPAGE { WORD lang, cp; } *tr = nullptr;
        UINT trLen = 0;
        if (::VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&tr, &trLen) && trLen >= 4) {
            wchar_t sub[128];
            swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
            wchar_t* val = nullptr;
            UINT vl = 0;
            if (::VerQueryValueW(buf.data(), sub, (LPVOID*)&val, &vl) && val)
                *desc = val;
        }
    }
    return true;
}



// ---------------------------------------------------------------------------
// Split a string into lines ('\n', ignoring '\r' so CRLF input is fine).
// Always returns at least one element, so callers can rely on lines[0].
// ---------------------------------------------------------------------------
std::vector<std::wstring> SplitLines(const std::wstring& text)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') { out.push_back(cur); cur.clear(); continue; }
        cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}
// ---------------------------------------------------------------------------
// SA-MP colour codes inside a string.
//   ~b~ ~g~ ~h~ ~l~ ~p~ ~r~ ~s~ ~w~   and  {RRGGBB} / {AARRGGBB}
//   ~n~ becomes a line break, any other ~x~ is left as literal text.
// ---------------------------------------------------------------------------
static bool IsHexDigit(wchar_t c)
{
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

static DWORD HexValue(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return (DWORD)(c - L'0');
    if (c >= L'a' && c <= L'f') return (DWORD)(c - L'a' + 10);
    return (DWORD)(c - L'A' + 10);
}

// The client's text renderer keeps one palette (gta_sa.exe 0xBAB22C, filled by
// CColour's constructor) and picks an entry per code. The values below are that
// palette, and a rendered /tdcodes page pinned down which entry each code uses -
// including that ~l~ is black, not grey.
static const DWORD kTextPal[] = {
    0x00B4191D,   // 0  red          (~r~)
    0x0036682C,   // 1  green        (~g~)
    0x00323C7F,   // 2  blue         (~b~)
    0x00ACCBF1,   // 3  pale blue
    0x00E1E1E1,   // 4  light grey
    0x00000000,   // 5  black        (~l~)
    0x00906210,   // 6  brown
    0x00A86EFC,   // 7  purple       (~p~)
    0x00969696,   // 8  grey
    0x00680F11,   // 9  dark red
    0x0026471F,   // 10 dark green
    0x00E2C063,   // 11 tan          (~y~)
    0x004A5A6B,   // 12 grey blue
    0x001419C8,   // 13 blue
    0x00FFFF00,   // 14 yellow
};

static D3DCOLOR Pal(int index) { return 0xFF000000u | kTextPal[index]; }

// ~h~ multiplies every channel by 1.5 and clamps at 255. That is literally what
// the client does (CFont::ParseToken, gta_sa.exe 0x718F00: color.r = min(r * 1.5f,
// 255.0f) for r, g and b), so repeated ~h~ compounds and truncates after every
// step: red's 25 becomes 37, 55, 82, 123, 184, and a screenshot of the client five
// steps up measures exactly (221, 159, 187) on its 221/255 screen scale. There is
// no per-colour cap - a code just runs out of headroom, which is why the docs
// quote 5 levels for red and 1 for white.
static D3DCOLOR LightenStep(D3DCOLOR c)
{
    auto step = [](DWORD v) { DWORD n = v + (v >> 1); return n > 255u ? 255u : n; };
    return (c & 0xFF000000u) | (step((c >> 16) & 0xFFu) << 16)
                            | (step((c >> 8) & 0xFFu) << 8)
                            | step(c & 0xFFu);
}

std::vector<TextRun> ParseColorRuns(const std::wstring& s, D3DCOLOR base, bool allowBrace)
{
    std::vector<TextRun> out;
    D3DCOLOR   cur = base;
    wchar_t    lastTag = 0;   // the client remembers the last colour code per line
    std::wstring acc;

    auto flush = [&]() {
        if (!acc.empty()) { out.push_back(TextRun{ acc, cur }); acc.clear(); }
    };

    // Applies one colour code to cur and returns whether it was one of ours; the
    // caller draws anything else literally. ~h~ is applied to the colour on the
    // spot rather than counted, because the client multiplies the current colour
    // per code and truncates after every step, so five ~h~ are not 1.5^5 applied
    // once.
    auto applyCode = [&](wchar_t k) -> bool {
        D3DCOLOR nc = 0;
        switch (k) {
        case L'b': nc = Pal(2);  break;   // blue
        case L'g': nc = Pal(1);  break;   // green
        case L'l': nc = Pal(5);  break;   // black; ~h~ on it stays black
        case L'p': nc = Pal(7);  break;   // purple
        case L'r': nc = Pal(0);  break;   // red
        case L'y': nc = Pal(11); break;   // tan, which is what the client draws
        case L's': nc = Pal(4);  break;   // light grey, not pure white
        case L'w': nc = Pal(4);  break;   // alias of ~s~
        case L'h': flush(); cur = LightenStep(cur); return true;
        default: return false;
        }
        flush();
        cur = nc;
        return true;
    };

    for (size_t i = 0; i < s.size();) {
        wchar_t c = s[i];

        if (c == L'~' && i + 2 < s.size() && s[i + 2] == L'~') {
            wchar_t k = s[i + 1];

            // The arrows are the client's own button sprites, not glyphs our fonts
            // could carry; leave a placeholder the layout turns into a sprite.
            if (k == L'u') { acc.push_back(kSymbolFirst + 0); i += 3; continue; }
            if (k == L'd') { acc.push_back(kSymbolFirst + 1); i += 3; continue; }
            if (k == L'<') { acc.push_back(kSymbolFirst + 2); i += 3; continue; }
            if (k == L'>') { acc.push_back(kSymbolFirst + 3); i += 3; continue; }

            // ~]~ is documented as an asterisk in GameText styles 3, 4 and 5 only,
            // but the client's own font parser has no case for it and a captured
            // vanilla GameText style 3 turns "star ~]~ end" into "STAR END". The
            // token is therefore dropped everywhere, like the client does.
            if (k == L']') { i += 3; continue; }

            // ~k~~KEYNAME~ : the real key binding lives in the client's controls,
            // we can only show the action name.
            if (k == L'k' && i + 3 < s.size() && s[i + 3] == L'~') {
                size_t end = s.find(L'~', i + 4);
                if (end != std::wstring::npos) {
                    for (size_t p = i + 4; p < end; p++) {
                        acc.push_back(s[p] == L'_' ? L' ' : s[p]);
                    }
                    i = end + 1;
                    continue;
                }
            }

            // A line break keeps the colour, and the client re-applies the last
            // colour code it saw at the start of the following line (it rebuilds
            // that line as "~tag~" + rest). So a trailing ~h~ lightens every
            // following line one step more, which is why the "x" in
            // "~r~~h~x~n~x" comes out white and not red.
            if (k == L'n') {
                flush();
                if (lastTag) applyCode(lastTag);
                acc.push_back(L'\n');
                i += 3;
                continue;
            }

            if (applyCode(k)) { lastTag = k; i += 3; continue; }
        }

        // Colour embedding {RRGGBB} is not part of the client's textdraw or
        // gametext syntax (open.mp colours list: "does not work in textdraws"), so
        // by default the braces are drawn as written, exactly like the client.
        // [textdraw] brace_colors = on enables the extension.
        if (allowBrace && c == L'{') {
            size_t rb = s.find(L'}', i + 1);
            if (rb != std::wstring::npos) {
                size_t n = rb - i - 1;
                bool ok = (n == 6 || n == 8);
                for (size_t k = i + 1; ok && k < rb; k++) if (!IsHexDigit(s[k])) ok = false;
                if (ok) {
                    DWORD v = 0;
                    for (size_t k = i + 1; k < rb; k++) v = (v << 4) | HexValue(s[k]);
                    if (n == 6) {
                        v |= 0xFF000000u;          // RRGGBB, fully opaque
                    } else {
                        // Eight digits are this mod's extension, not a client
                        // feature; they are read in the same order as every other
                        // SA-MP colour, RRGGBBAA, and converted to the device's
                        // AARRGGBB here.
                        const DWORD rgb = (v >> 8) & 0x00FFFFFFu;
                        const DWORD a   = v & 0xFFu;
                        v = (a << 24) | rgb;
                    }
                    flush();
                    cur = v;
                    i = rb + 1;
                    continue;
                }
            }
        }

        acc.push_back(c);
        i++;
    }
    flush();
    return out;
}
// Cuts a colour run into pieces the renderer can draw: text is split by face,
// and every inline symbol placeholder becomes a piece of its own, because the
// client draws ~u~ ~d~ ~<~ ~>~ from its own button textures and advances the pen
// by the width of that square, not by a glyph width.
// Measured against the client with one probe line per face (see README, "Quirks"):
// face 2 draws a star for ']' and nothing at all for '[', while faces 1 and 3 keep
// the plain brackets. Everything else is left alone - the point is to draw the same
// symbol the client draws, not to scramble the rest of the punctuation.
static const struct { int font; wchar_t from; wchar_t to; } kClientGlyphs[] = {
    { 2, L'[', 0x0000 },       // no glyph: the client draws nothing
    { 2, L']', 0x2605 },       // U+2605 BLACK STAR, which is what the client shows
};

wchar_t ClientGlyphFor(int fontId, wchar_t ch)
{
    if (!g_cfg.clientGlyphs) return ch;
    for (size_t i = 0; i < _countof(kClientGlyphs); i++) {
        if (kClientGlyphs[i].font == fontId && kClientGlyphs[i].from == ch) {
            return kClientGlyphs[i].to;
        }
    }
    return ch;
}

void SplitRunIntoPieces(const std::wstring& text, int pxHeight, int fontId, std::vector<DrawPiece>& out)
{
    std::wstring acc;
    auto flush = [&]() {
        if (acc.empty()) return;
        std::vector<TextSpan> spans = SplitByFont(acc, fontId);
        for (size_t i = 0; i < spans.size(); i++) {
            DrawPiece p;
            p.text  = spans[i].text;
            p.font  = spans[i].font;
            p.width = MeasureTextWidth(spans[i].text, pxHeight, spans[i].font);
            out.push_back(p);
        }
        acc.clear();
    };

    for (size_t i = 0; i < text.size(); i++) {
        const int slot = SymbolSlot(text[i]);
        if (!slot) {
            const wchar_t sub = ClientGlyphFor(fontId, text[i]);
            if (sub) acc.push_back(sub);      // 0 = the client draws nothing here
            continue;
        }
        flush();
        DrawPiece p;
        p.width  = SymbolSizePx(pxHeight);
        p.symbol = slot;
        out.push_back(p);
    }
    flush();
}

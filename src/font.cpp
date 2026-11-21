// ---------------------------------------------------------------------------
// font.cpp - parse TrueType / OpenType / TTC name tables ourselves, then load
//            the font privately through GDI
//   - tag constants 'ttcf' = 0x66637474, 'name' = 0x656D616E
//   - AddFontResourceExW(path, FR_PRIVATE = 0x10, 0)
//   - GetGlyphIndicesW(<probe>, GGI_MARK_NONEXISTING_GLYPHS)
// ---------------------------------------------------------------------------
#include "common.h"
#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// Big-endian readers
// ---------------------------------------------------------------------------
static inline WORD  RD16(const BYTE* p) { return (WORD)((p[0] << 8) | p[1]); }
static inline DWORD RD32(const BYTE* p) { return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | p[3]; }

static bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>& out)
{
    FILE* fp = nullptr;
    ::_wfopen_s(&fp, path.c_str(), L"rb");
    if (!fp) return false;
    ::fseek(fp, 0, SEEK_END);
    long n = ::ftell(fp);
    ::fseek(fp, 0, SEEK_SET);
    if (n <= 0 || n > (long)(64u * 1024u * 1024u)) { ::fclose(fp); return false; }
    out.resize((size_t)n);
    bool ok = ::fread(out.data(), 1, (size_t)n, fp) == (size_t)n;
    ::fclose(fp);
    return ok;
}

// ---------------------------------------------------------------------------
// Read the family name (nameID 1 / 4) from an sfnt, TTC supported
// ---------------------------------------------------------------------------
static std::wstring ReadNameFromFont(const std::vector<BYTE>& f, size_t fontOff)
{
    if (fontOff + 12 > f.size()) return std::wstring();
    const BYTE* base = f.data() + fontOff;
    DWORD tag = RD32(base);
    if (tag != 0x00010000 && tag != 0x4F54544F /*OTTO*/ && tag != 0x74727565 /*true*/)
        return std::wstring();

    WORD numTables = RD16(base + 4);
    DWORD nameOff = 0;
    for (WORD i = 0; i < numTables; i++) {
        const BYTE* rec = base + 12 + (size_t)i * 16;
        if (rec + 16 > f.data() + f.size()) break;
        if (RD32(rec) == 0x6E616D65 /*'name'*/) {
            nameOff = RD32(rec + 8);
            break;
        }
    }
    if (!nameOff || fontOff >= f.size() || nameOff > f.size() - fontOff - 6) return std::wstring();

    const BYTE* nt = f.data() + fontOff + nameOff;
    WORD count = RD16(nt + 2);
    WORD strOff = RD16(nt + 4);

    // nameID 1 is the family ("Arial"), nameID 4 the full name ("Arial Bold").
    // GDI matches faces by family, so the family has to win: a full name with a
    // style suffix in it does not exist as a face and CreateFont would silently
    // substitute something else.
    std::wstring family;  // nameID 1, preferred
    std::wstring full;    // nameID 4, only when a font has no family record
    for (WORD i = 0; i < count; i++) {
        const BYTE* r = nt + 6 + (size_t)i * 12;
        if (r + 12 > f.data() + f.size()) break;
        WORD platform = RD16(r), encoding = RD16(r + 2);
        WORD nameID = RD16(r + 6);
        WORD len = RD16(r + 8);
        WORD off = RD16(r + 10);
        if (nameID != 1 && nameID != 4) continue;
        const BYTE* s = nt + strOff + off;
        if (s + len > f.data() + f.size()) continue;

        std::wstring val;
        if (platform == 3 || platform == 0) {
            val.assign(len / 2, L'\0');
            for (WORD k = 0; k < len / 2; k++)
                val[k] = (wchar_t)((s[k * 2] << 8) | s[k * 2 + 1]);
        } else if (platform == 1) {
            val.assign(len, L'\0');
            for (WORD k = 0; k < len; k++) val[k] = (wchar_t)s[k];
        } else {
            continue;
        }
        if (nameID == 1 && family.empty()) family = val;
        if (nameID == 4 && full.empty()) full = val;
    }
    return family.empty() ? full : family;
}

std::wstring ParseFontFamily(const std::wstring& file, int ttcIndex)
{
    std::vector<BYTE> f;
    if (!ReadFileBytes(file, f) || f.size() < 12) return std::wstring();

    size_t fontOff = 0;
    if (RD32(f.data()) == 0x74746366 /*'ttcf'*/) {
        const DWORD num = RD32(f.data() + 8);
        if (!num || num > (f.size() - 12) / 4) return std::wstring();

        // A TTC face can have an empty name table (simsun.ttc does: face 0 reports
        // format 0 with no records). The requested index is tried first, then every
        // other face, so a collection with one silent face still works.
        const int want = (ttcIndex >= 0 && (DWORD)ttcIndex < num) ? ttcIndex : 0;
        std::wstring first;
        for (DWORD pass = 0; pass < num; pass++) {
            const int idx = (pass == 0) ? want : (int)((want + pass) % num);
            const size_t off = RD32(f.data() + 12 + (size_t)idx * 4);
            if (off >= f.size()) continue;
            std::wstring n = ReadNameFromFont(f, off);
            if (n.empty()) continue;
            if (pass == 0) return n;
            if (first.empty()) first = n;
        }
        return first;
    }
    (void)ttcIndex;
    return ReadNameFromFont(f, fontOff);
}

// ---------------------------------------------------------------------------
// Glyph coverage check; the probe sample depends on the active code page
//   probe == nullptr means "do not check, accept the font"
// ---------------------------------------------------------------------------
static bool FaceHasGlyphs(const std::wstring& face, const wchar_t* probe)
{
    if (face.empty()) return false;
    if (!probe || !*probe) return true;
    int probeLen = (int)::wcslen(probe);
    if (probeLen <= 0 || probeLen > 16) return true;

    HDC dc = ::CreateCompatibleDC(nullptr);
    if (!dc) return false;

    LOGFONTW lf = {};
    lf.lfHeight  = -20;
    lf.lfWeight  = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    ::lstrcpynW(lf.lfFaceName, face.c_str(), 32);

    HFONT fn = ::CreateFontIndirectW(&lf);
    if (!fn) { ::DeleteDC(dc); return false; }
    HGDIOBJ old = ::SelectObject(dc, fn);

    WORD idx[16] = { 0 };
    DWORD r = ::GetGlyphIndicesW(dc, probe, probeLen, idx, GGI_MARK_NONEXISTING_GLYPHS);
    bool ok = (r != (DWORD)-1);
    if (ok) {
        for (int i = 0; i < probeLen; i++) if (idx[i] == 0xFFFF) { ok = false; break; }
    }

    ::SelectObject(dc, old);
    ::DeleteObject(fn);
    ::DeleteDC(dc);
    return ok;
}

// Probe sample for the current code page; UTF-8 prefers a wide (CJK) font
static const wchar_t* CurrentProbe()
{
    UINT cp = ResolveCodePage();
    if (cp == CP_UTF8) return L"\u4E2D\u6587\u6D4B\u8BD5\u6B66\u4FA0";
    return ProbeStringForCp(cp);
}

// ---------------------------------------------------------------------------
// Face lookup
//
// CreateFont silently substitutes a face that does not exist, so asking it is
// useless; EnumFontFamiliesEx with a face name only reports exact matches.
// ---------------------------------------------------------------------------
static int CALLBACK CountFaceProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp)
{
    ++*(int*)lp;
    return 0;   // one match is enough
}

static bool FaceExists(const std::wstring& face)
{
    if (face.empty() || face.size() >= LF_FACESIZE) return false;

    HDC dc = ::GetDC(nullptr);
    if (!dc) return false;

    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    ::lstrcpynW(lf.lfFaceName, face.c_str(), LF_FACESIZE);

    int found = 0;
    ::EnumFontFamiliesExW(dc, &lf, CountFaceProc, (LPARAM)&found, 0);
    ::ReleaseDC(nullptr, dc);
    return found > 0;
}

// ---------------------------------------------------------------------------
// The four TextDrawFont faces, best candidate first
//
// The original renderer has: 0 = blackletter, 1 = bold sans, 2 = Bank Gothic
// (wide capitals), 3 = Pricedown. Stock Windows only carries the last two at
// best, so every role ends with a shape-appropriate substitute; an explicit
// family or font file in the INI wins over all of this.
// ---------------------------------------------------------------------------
static const wchar_t* kRoleCandidates[kFontRoleCount][8] = {
    { L"Old English Text MT", L"Blackadder ITC", L"Cloister Black", L"UnifrakturCook",
      L"UnifrakturMaguntia", L"Grenze Gotisch", L"Pirata One", L"Uncial Antiqua" },
    { L"Arial", L"Helvetica", L"Liberation Sans", L"Segoe UI", L"Microsoft Sans Serif" },
    { L"Bahnschrift", L"Bank Gothic", L"Microgramma", L"Eurostile",
      L"Franklin Gothic Medium", L"Arial" },
    { L"Impact", L"Haettenschweiler", L"Arial Black", L"Arial" },
};

static const wchar_t* kRoleFallback[kFontRoleCount] = {
    L"Times New Roman",        // closest stock face to a blackletter
    L"Arial",
    L"Franklin Gothic Medium",
    L"Impact",
};

static bool ResolvePath(std::wstring& inout);   // defined below
static bool FileExists(const std::wstring& p);  // defined below

static bool IsFontFile(const std::wstring& s)
{
    if (s.size() < 5) return false;
    std::wstring ext;
    for (size_t i = s.size() - 4; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        ext.push_back(c);
    }
    return ext == L".ttf" || ext == L".otf" || ext == L".ttc";
}

// Every file registered with AddFontResourceExW, so unload can undo it: the call
// keeps the file mapped (and locked) for the process lifetime otherwise.
static std::vector<std::wstring> g_loadedFonts;

void FontsUnload()
{
    for (size_t i = 0; i < g_loadedFonts.size(); i++)
        ::RemoveFontResourceExW(g_loadedFonts[i].c_str(), FR_PRIVATE, nullptr);
    g_loadedFonts.clear();
}

static bool LoadFaceFile(const std::wstring& path, std::wstring* faceOut)
{
    // [font] face picks the family by hand: some collections (simsun.ttc) carry no
    // usable name record, and then only the caller knows the right one.
    std::wstring fam = g_cfg.fontFace.empty() ? ParseFontFamily(path, g_cfg.collectionIndex)
                                              : g_cfg.fontFace;
    if (fam.empty()) {
        LogW(L"font file %s: cannot read the family name (set [font] face = to name it)", path.c_str());
        return false;
    }
    if (!::AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr)) {
        LogW(L"font file %s: AddFontResourceEx failed (%lu)", path.c_str(), ::GetLastError());
        return false;
    }
    g_loadedFonts.push_back(path);   // removed in FontsUnload, also unlocks the file
    *faceOut = fam;
    return true;
}

static void ResolveRoleFace(int role)
{
    // 1) the INI wins: fontN = <installed family> or fontN = <font file>
    const std::wstring& want = g_cfg.face[role];

    if (!want.empty()) {
        if (IsFontFile(want)) {
            std::wstring p = want;
            if (ResolvePath(p) && LoadFaceFile(p, &g_fontFaces[role])) {
                LogW(L"font%d: %s", role, p.c_str());
                return;
            }
            LogW(L"font%d: cannot load '%s', falling back to auto", role, want.c_str());
        } else if (FaceExists(want)) {
            g_fontFaces[role] = want;
            LogW(L"font%d: %s (installed face)", role, want.c_str());
            return;
        } else {
            LogW(L"font%d: face '%s' is not installed, falling back to auto", role, want.c_str());
        }
    }

    // 2) a font file dropped next to the plugin needs no INI edit:
    //    samp-unicode-font0.ttf ... or the shorter font0.ttf
    {
        static const wchar_t* kExt[] = { L".ttf", L".otf", L".ttc" };
        for (int e = 0; e < 3; e++) {
            wchar_t name[80], brief[32];
            _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s-font%d%s", g_pluginStem.c_str(), role, kExt[e]);
            swprintf_s(brief, L"font%d%s", role, kExt[e]);
            const wchar_t* both[] = { name, brief };
            for (const wchar_t* n : both) {
                std::wstring p = g_gameDir + n;
                if (FileExists(p) && LoadFaceFile(p, &g_fontFaces[role])) {
                    LogW(L"font%d: %s", role, p.c_str());
                    return;
                }
            }
        }
    }

    // 3) closest installed face, 4) shape-appropriate fallback
    for (int i = 0; i < 8; i++) {
        const wchar_t* cand = kRoleCandidates[role][i];
        if (cand && *cand && FaceExists(cand)) { g_fontFaces[role] = cand; return; }
    }
    if (FaceExists(kRoleFallback[role])) { g_fontFaces[role] = kRoleFallback[role]; return; }

    g_fontFaces[role] = g_fontFaces[kFontDefault];
}

// ---------------------------------------------------------------------------
// Candidate paths
// ---------------------------------------------------------------------------
static void AddCandidate(std::vector<std::wstring>& v, const std::wstring& dir,
                         const std::wstring& name)
{
    std::wstring p = dir + name;
    if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
}

static bool FileExists(const std::wstring& p)
{
    return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// samp-unicode-font0.ttf (and the shorter font0.ttf) are the per-TextDrawFont
// faces; they carry no CJK, so the display-font scan must not pick them up.
static bool IsRoleFile(const std::wstring& name)
{
    const std::wstring stem = g_pluginStem + L"-font";
    if (name.size() >= stem.size() && _wcsnicmp(name.c_str(), stem.c_str(), stem.size()) == 0)
        return true;
    if (name.size() >= 5 && _wcsnicmp(name.c_str(), L"font", 4) == 0 &&
        name[4] >= L'0' && name[4] <= L'9')
        return true;
    return false;
}

static bool ResolvePath(std::wstring& inout)
{
    if (inout.empty()) return false;

    const bool absolute = inout.find(L':') != std::wstring::npos ||
                          (!inout.empty() && inout[0] == L'\\');
    if (absolute) return FileExists(inout);

    // A relative path is looked up next to the plugin first - with modloader
    // that is ...\GTA San Andreas\modloader\samp-unicode\, which is where users
    // drop their fonts - then in the game folder (next to gta_sa.exe, for a
    // plain install where the .asi sits in the root), then in the working
    // directory. The log always prints the resolved absolute path.
    static std::wstring exeDir;
    if (exeDir.empty()) {
        wchar_t buf[MAX_PATH] = {};
        if (::GetModuleFileNameW(nullptr, buf, MAX_PATH)) {
            std::wstring p = buf;
            size_t slash = p.find_last_of(L"\\/");
            if (slash != std::wstring::npos) exeDir = p.substr(0, slash + 1);
        }
    }

    const std::wstring dirs[] = { g_gameDir, exeDir };
    for (const auto& dir : dirs) {
        if (dir.empty()) continue;
        std::wstring p = dir + inout;
        if (FileExists(p)) { inout = p; return true; }
    }
    return FileExists(inout);   // finally as given, i.e. the working directory
}

// ---------------------------------------------------------------------------
// Default (fallback) face: the one that has to cover the station charset
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Face preference
//
// One table instead of three: each entry says which scripts it serves and whether
// it is a serif face, and the order *is* the preference order. A face that does not
// carry the script produces wrong glyph shapes (a Japanese page rendered with
// simplified forms, Korean with no Hangul at all), so the script-specific entries
// come first and are filtered by the active code page. Inside one script the order
// is: modern quality first, then what the OS ships, then the legacy faces - a font
// that is installed wins over the order anyway, the tail only matters for the
// machines that have several.
// ---------------------------------------------------------------------------
enum : unsigned {
    kSc    = 1u << 0,   // simplified Chinese
    kTc    = 1u << 1,   // traditional Chinese
    kJa    = 1u << 2,   // Japanese
    kKo    = 1u << 3,   // Korean
    kLatin = 1u << 4,
    kCjk   = kSc | kTc | kJa | kKo,
};

struct FaceEntry {
    const wchar_t* name;
    unsigned       scripts;
    bool           serif;
};

static const FaceEntry kFaces[] = {
    // --- script specific: wrong shapes are worse than a different look ---
    { L"Yu Gothic",             kJa,          false },
    { L"Meiryo",                kJa,          false },
    { L"MS Gothic",             kJa,          true  },
    { L"Malgun Gothic",         kKo,          false },
    { L"Gulim",                 kKo,          true  },
    { L"Microsoft JhengHei",    kTc,          false },
    { L"MingLiU",               kTc,          true  },

    // --- simplified Chinese, sans: quality first, then the OS faces ---
    { L"Noto Sans SC",          kSc,          false },
    { L"Noto Sans CJK SC",      kSc,          false },   // same face, other install name
    { L"Source Han Sans SC",    kSc,          false },
    { L"Source Han Sans CN",    kSc,          false },
    { L"Microsoft YaHei",       kSc,          false },
    { L"Microsoft YaHei UI",    kSc,          false },
    { L"PingFang SC",           kSc,          false },
    { L"Hiragino Sans GB",      kSc,          false },
    { L"HarmonyOS Sans SC",     kSc,          false },
    { L"MiSans",                kSc,          false },
    { L"WenQuanYi Micro Hei",   kSc,          false },
    { L"SimHei",                kSc,          false },
    { L"DengXian",              kSc,          false },

    // --- simplified Chinese, serif ---
    { L"Noto Serif SC",         kSc,          true  },
    { L"Source Han Serif SC",   kSc,          true  },
    { L"Source Han Serif CN",   kSc,          true  },
    { L"Songti SC",             kSc,          true  },
    { L"SimSun",                kSc,          true  },
    { L"NSimSun",               kSc,          true  },
    { L"KaiTi",                 kSc,          true  },
    { L"FangSong",              kSc | kTc,    true  },

    // --- Latin base, also the last resort for a CJK page ---
    { L"Arial",                 kLatin,       false },
    { L"Tahoma",                kLatin,       false },
    { L"Segoe UI",              kLatin,       false },
    { L"Microsoft Sans Serif",  kLatin,       false },
    { L"Verdana",               kLatin,       false },
    { L"Calibri",               kLatin,       false },
    { L"Times New Roman",       kLatin,       true  },

    // --- wide coverage, used only when nothing else fits ---
    { L"Arial Unicode MS",      kLatin | kCjk, false },
};

// Code page -> the scripts that must not be rendered with a fallback face.
static unsigned ScriptsForCodePage(UINT cp)
{
    switch (cp) {
        case 936:  return kSc;
        case 950:  return kTc;
        case 932:  return kJa;
        case 949:  return kKo;
        case 65001: return kSc;      // UTF-8: the content decides, SC is the safest guess
        default:   return kLatin;
    }
}

// First face in the table that serves one of `scripts`, matches the requested style
// and really has glyphs for `probe`. Pass 2 relaxes the style so a sans face is used
// when the page asked for a serif one and none is installed.
static const wchar_t* PickFace(unsigned scripts, bool wantSerif, const wchar_t* probe)
{
    for (int pass = 0; pass < (wantSerif ? 2 : 1); pass++) {
        for (size_t i = 0; i < _countof(kFaces); i++) {
            if (StopRequested()) return nullptr;   // unload requested mid-probe
            const FaceEntry& e = kFaces[i];
            if (!(e.scripts & scripts)) continue;
            if (pass == 0 && e.serif != wantSerif) continue;
            if (FaceHasGlyphs(e.name, probe)) return e.name;
        }
    }
    return nullptr;
}

// [font] family takes a list the way CSS does: "Noto Sans SC, Microsoft YaHei,
// sans-serif" - the first entry whose face exists and covers the charset wins.
// Entries may be quoted ("Source Han Sans SC"), including names with a comma
// inside the quotes, so the split ignores separators while inside a quote.
static std::vector<std::wstring> SplitFamilyList(const std::wstring& s)
{
    std::vector<std::wstring> out;
    std::wstring part;
    wchar_t quote = 0;

    auto flush = [&]() {
        size_t b = part.find_first_not_of(L" \t");
        size_t e = part.find_last_not_of(L" \t");
        if (b == std::wstring::npos) { part.clear(); return; }
        std::wstring one = part.substr(b, e - b + 1);
        // strip one matching pair of surrounding quotes
        if (one.size() >= 2 && (one.front() == L'"' || one.front() == L'\'') &&
            one.back() == one.front()) {
            one = one.substr(1, one.size() - 2);
        }
        if (!one.empty()) out.push_back(one);
        part.clear();
    };

    for (wchar_t c : s) {
        if (quote) {
            part.push_back(c);
            if (c == quote) quote = 0;
            continue;
        }
        if (c == L'"' || c == L'\'') { quote = c; part.push_back(c); continue; }
        if (c == L',') { flush(); continue; }
        part.push_back(c);
    }
    flush();
    return out;
}

static bool IsGenericSans(const std::wstring& n)
{
    return n == L"sans-serif" || n == L"system-ui" || n == L"ui-sans-serif";
}
static bool IsGenericSerif(const std::wstring& n)
{
    return n == L"serif" || n == L"ui-serif";
}

static bool ResolveDefaultFace(const wchar_t* probe)
{
    if (!g_cfg.family.empty()) {
        const std::vector<std::wstring> list = SplitFamilyList(g_cfg.family);
        for (const std::wstring& want : list) {
            if (IsGenericSans(want) || IsGenericSerif(want)) {
                const unsigned sc = ScriptsForCodePage(ResolveCodePage());
                const wchar_t* pick = PickFace(sc | kCjk | kLatin, IsGenericSerif(want), probe);
                if (pick) {
                    g_fontFaces[kFontDefault] = pick;
                    LogW(L"display font: %s (generic %s)", pick, want.c_str());
                    return true;
                }
            } else if (FaceExists(want) && FaceHasGlyphs(want, probe)) {
                g_fontFaces[kFontDefault] = want;
                LogW(L"display font: %s (from [font] family)", want.c_str());
                return true;
            }
        }
        LogW(L"[font] family = %s matched nothing, using the automatic order", g_cfg.family.c_str());
    }

    std::vector<std::wstring> cands;

    // 1) font file given in the INI
    if (!g_cfg.file.empty()) {
        std::wstring p = g_cfg.file;
        if (ResolvePath(p)) cands.push_back(p);
        else LogW(L"cannot read font file: %s", g_cfg.file.c_str());
    }
    // 2) automatic candidates: our own name, then generic file names
    static const wchar_t* kExt[] = { L".ttf", L".otf", L".ttc" };
    for (int i = 0; i < 3; i++) {
        AddCandidate(cands, g_gameDir, g_pluginStem + kExt[i]);
        AddCandidate(cands, g_gameDir, std::wstring(L"samp-unicode") + kExt[i]);
    }
    static const wchar_t* kNames[] = {
        L"font.ttf", L"font.otf", L"font.ttc",
        L"gametext.ttf", L"gametext.otf", L"gametext.ttc",
    };
    for (auto n : kNames) AddCandidate(cands, g_gameDir, n);

    // 3) directory scan: first .ttf / .otf / .ttc found
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileW((g_gameDir + L"*.ttf").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            if (!IsRoleFile(fd.cFileName)) AddCandidate(cands, g_gameDir, fd.cFileName);
            ::FindClose(h);
        }
        h = ::FindFirstFileW((g_gameDir + L"*.otf").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            if (!IsRoleFile(fd.cFileName)) AddCandidate(cands, g_gameDir, fd.cFileName);
            ::FindClose(h);
        }
        h = ::FindFirstFileW((g_gameDir + L"*.ttc").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            if (!IsRoleFile(fd.cFileName)) AddCandidate(cands, g_gameDir, fd.cFileName);
            ::FindClose(h);
        }
    }

    for (auto& path : cands) {
        if (!FileExists(path)) continue;
        std::wstring fam = ParseFontFamily(path, g_cfg.collectionIndex);
        if (fam.empty()) {
            LogDbg(L"font %s: cannot read the family name (set Face= in the INI)", path.c_str());
            continue;
        }
        // [font] family is a CSS-like list ("Noto Sans SC", sans-serif) and is not a face
    // name: using it here made FaceHasGlyphs reject a perfectly good file.

        int added = ::AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr);
        if (!added) {
            LogW(L"private font load failed: %s (error %lu)", path.c_str(), ::GetLastError());
            continue;
        }
        if (!FaceHasGlyphs(fam, probe)) {
            LogW(L"font %s family '%s' has no glyphs for this charset, skipping", path.c_str(), fam.c_str());
            ::RemoveFontResourceExW(path.c_str(), FR_PRIVATE, nullptr);
            continue;
        }
        g_fontFaces[kFontDefault] = fam;
        LogW(L"font file=%s face=%s index=%d", path.c_str(), fam.c_str(), g_cfg.collectionIndex);
        LogW(L"display font: %s", g_fontFaces[kFontDefault].c_str());
        return true;
    }

    // 4) installed faces: the scripts of the active code page first, then anything
    // that covers the charset, then a Latin face that at least shows something.
    UINT cp = ResolveCodePage();
    const unsigned sc = ScriptsForCodePage(cp);

    const wchar_t* pick = PickFace(sc, false, probe);
    if (!pick) pick = PickFace(kCjk | kLatin, false, probe);
    if (pick) {
        g_fontFaces[kFontDefault] = pick;
        LogW(L"configured font unusable, falling back to a system font");
        LogW(L"display font: %s", g_fontFaces[kFontDefault].c_str());
        return true;
    }

    // 5) last resort: a face that can render basic Latin (avoids a blank screen)
    const wchar_t* latin = PickFace(kLatin | kCjk, false, L"\u00C1\u00C9\u00CD\u00D3\u00DA");
    if (latin) {
        g_fontFaces[kFontDefault] = latin;
        LogW(L"no font covers charset %u, falling back to %s (some glyphs may be boxes)",
             cp, latin);
        return true;
    }

    LogW(L"no usable font found, text may render as boxes");
    return false;
}

// ---------------------------------------------------------------------------
// Main entry point: the fallback face first, then the four TextDrawFont faces
// ---------------------------------------------------------------------------
bool LoadFont()
{
    bool ok = ResolveDefaultFace(CurrentProbe());

    for (int role = 0; role < kFontRoleCount; role++) ResolveRoleFace(role);

    LogW(L"fonts: fallback=%s | 0=%s 1=%s 2=%s 3=%s",
         g_fontFaces[kFontDefault].c_str(),
         g_fontFaces[0].c_str(), g_fontFaces[1].c_str(),
         g_fontFaces[2].c_str(), g_fontFaces[3].c_str());
    return ok;
}




// ---------------------------------------------------------------------------
// hooks.cpp - targeted byte patching and the detours
//
// TextDraw: the client dispatches through a 19-byte thunk; only the rel32 operand
//   of its second jmp is rewritten, so no instruction boundary moves and no
//   trampoline is needed. The signature and the operand offsets are in common.h.
// GameText: the prologue is copied into a trampoline, then the entry is overwritten
//   with a jmp to our detour.
// Menus, the scoreboard flag and the ~k~ expander are located the same way: by
//   structure, never by a hard-coded address (see the comments at each site).
// ---------------------------------------------------------------------------
#include "common.h"
#include <tlhelp32.h>
#include <cstdio>

// forward declarations (detours are implemented below)
static void __fastcall TextDrawDetour(void* self, void* unused);
static void __stdcall  GameTextDetour(char* text, int time, int style);
static const char*      ExpandKeys(const char* text, char* buf, size_t size);   // ~k~~KEY~ via the client

// A rel32 jump can only reach +/-2 GB. VirtualAlloc may hand back a trampoline
// anywhere in the 32-bit address space, so check before encoding the jump.
static bool FitsRel32(const void* from, const void* to, const wchar_t* what)
{
    const ptrdiff_t d = (const BYTE*)to - ((const BYTE*)from + 5);
    if (d >= -0x7FFF0000LL && d <= 0x7FFF0000LL) return true;
    LogW(L"%s out of rel32 reach (%p -> %p), hook skipped", what, from, to);
    return false;
}
// ---------------------------------------------------------------------------
// Thread freezing: suspend the others, then patch the bytes
// ---------------------------------------------------------------------------
static std::vector<HANDLE> g_frozen;

static void FreezeThreads()
{
    g_frozen.clear();
    DWORD pid = ::GetCurrentProcessId();
    DWORD tid = ::GetCurrentThreadId();
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    // Collect first, suspend afterwards. Suspending inside the loop can leave a
    // thread frozen while it holds the process heap or the CRT lock, and the next
    // allocation (a std::vector growth, for instance) would deadlock the install.
    std::vector<HANDLE> threads;
    threads.reserve(64);
    THREADENTRY32 te = { sizeof(te) };
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == tid) continue;
            HANDLE th = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (th) threads.push_back(th);
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);

    for (HANDLE th : threads) {
        if (::SuspendThread(th) != (DWORD)-1) g_frozen.push_back(th);
        else ::CloseHandle(th);
    }
}

static void UnfreezeThreads()
{
    for (auto h : g_frozen) { ::ResumeThread(h); ::CloseHandle(h); }
    g_frozen.clear();
}

// ---------------------------------------------------------------------------
static bool WriteMemory(void* addr, const void* data, size_t len)
{
    DWORD old = 0;
    if (!::VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    ::memcpy(addr, data, len);
    ::VirtualProtect(addr, len, old, &old);
    ::FlushInstructionCache(::GetCurrentProcess(), addr, len);
    return true;
}

// ---------------------------------------------------------------------------
// TextDraw
// ---------------------------------------------------------------------------
typedef void(__fastcall* TextDrawFn)(void* self, void* unused);
static TextDrawFn g_origTextDraw = nullptr;

// Depth of our own TextDraw draw calls. The menu text hook must not take over a
// string this path is already handling: the native call still walks the text
// (with the colours cleared), and if that string is non-ASCII and a menu is
// open the menu hook would blank it and draw a second copy in the menu's style.
static volatile LONG g_textDrawDepth = 0;

static void CallOriginalTextDraw(void* self)
{
    ::InterlockedIncrement(&g_textDrawDepth);
    g_origTextDraw(self, nullptr);
    ::InterlockedDecrement(&g_textDrawDepth);
}
static BYTE*      g_tdThunk = nullptr;
static DWORD      g_tdOrigRel32 = 0;
static bool       g_tdHooked = false;

// 19-byte signature (mask: 'x' must match, '?' is a wildcard)
static const BYTE kThunkPat[19] = {
    0x83, 0xB9, 0x00, 0x00, 0x00, 0x00, 0xFF,
    0x74, 0x05,
    0xE9, 0x00, 0x00, 0x00, 0x00,
    0xE9, 0x00, 0x00, 0x00, 0x00
};
static const char kThunkMask[19] = {
    'x', 'x', '?', '?', 'x', 'x', 'x',
    'x', 'x',
    'x', '?', '?', '?', '?',
    'x', '?', '?', '?', '?'
};

static DWORD SwapRB(DWORD c)
{
    return (c & 0xFF00FF00u) | ((c & 0x000000FFu) << 16) | ((c >> 16) & 0x000000FFu);
}

// function pointer -> byte address (union avoids a function-to-data pointer cast)
static BYTE* AddrOfTdDetour()
{
    union { void(__fastcall* f)(void*, void*); BYTE* p; } u;
    u.f = &TextDrawDetour;
    return u.p;
}

static BYTE* AddrOfGtDetour()
{
    union { void(__stdcall* f)(char*, int, int); BYTE* p; } u;
    u.f = &GameTextDetour;
    return u.p;
}

// The client lays a line out one glyph cell per *drawn* character; colour and
// style codes (~r~, ~h~, {RRGGBB}) take no space at all. Counting source bytes
// instead made a line full of codes far too wide - measured against the client
// on the same draw: +16% on /tdlight, +22% on /tdglyphs.
static int CountDrawnGlyphs(const char* text)
{
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)text; p && *p; ) {
        if (*p == '~') {                                  // ~x~ / ~xxx~ tag
            const unsigned char* q = p + 1;
            if (*q == '~') q++;
            else { while (*q && *q != '~') q++; if (*q) q++; }
            p = q;
            continue;
        }
        if (g_cfg.braceColors && *p == '{') {             // {RRGGBB} / {RRGGBBAA}
            const unsigned char* q = p + 1;
            int hex = 0;
            while (q[hex] && ((q[hex] >= '0' && q[hex] <= '9') ||
                              (q[hex] >= 'a' && q[hex] <= 'f') ||
                              (q[hex] >= 'A' && q[hex] <= 'F'))) hex++;
            if (q[hex] == '}' && (hex == 6 || hex == 8)) { p = q + hex + 1; continue; }
        }
        if (*p >= 0x20) n++;
        p++;
    }
    return n;
}

// The width the client gives the line: one cell per drawn glyph, plus the square
// every arrow or button symbol reserves. The cell is letterWidth in the same
// 640-wide space the letter size uses, divided by the factor the client's own
// advances show (its digits move ~30 of that unit, letters and spaces ~36, so 33
// is the middle). Symbols keep the width our renderer already gives them.
static float WidthScale(const char* text, float letterW, int screenW, float symbolsPx, int measuredTotal)
{
    if (!g_cfg.respectLetterWidth || !text || letterW <= 0.0f || measuredTotal <= 0) return 1.0f;

    int glyphs = CountDrawnGlyphs(text);
    if (glyphs <= 0 && symbolsPx <= 0.0f) return 1.0f;

    static const float kCellDivisor = 33.0f;
    float cell   = letterW * (float)screenW / kCellDivisor;
    float target = cell * (float)glyphs + symbolsPx;

    float sx = target / (float)measuredTotal;
    if (sx < 0.5f) sx = 0.5f;
    if (sx > 2.0f) sx = 2.0f;
    return sx;
}
// ---- detour ---------------------------------------------------------------
// Hide the client's own glyphs for one draw, then put everything back.
//
// The string is never touched: the glyphs stay where they are, the client still
// measures them, and only their alpha goes to zero. That keeps the client's own hit
// rectangle (a selectable textdraw) and its box exactly as the server set them up.
//
// In selection mode the client redraws a selectable textdraw with its own highlight
// colour (+0x9D1 flag / +0x9D2 colour, both written by the client, not by the
// server) and that pass ignores the normal text colour, so that slot is cleared too.
static void HideTextAndCallOriginal(void* self, char* obj)
{
    DWORD* hl = obj[samp::kOffHighlight] ? (DWORD*)(obj + samp::kOffHighlightColor) : nullptr;
    DWORD  hlSave = hl ? *hl : 0;
    if (hl) *hl &= 0x00FFFFFFu;

    DWORD* col1 = (DWORD*)(obj + samp::kOffColor);
    DWORD* col2 = (DWORD*)(obj + samp::kOffColor2);
    DWORD  save1 = *col1, save2 = *col2;

    *col1 &= 0x00FFFFFFu;
    *col2 &= 0x00FFFFFFu;
    CallOriginalTextDraw(self);
    *col1 = save1;
    *col2 = save2;
    if (hl) *hl = hlSave;
}

// The client redraws every selectable textdraw while a selection is active, so it
// can highlight several of them in one frame. Remember a few, not one: with a single
// slot the plain pass of the others looked unhighlighted and their text was drawn
// twice, once in each colour.
struct HlMark { void* obj; DWORD stamp; };
static HlMark s_hl[8] = {};
static int    s_hlNext = 0;

static bool RecentlyHighlighted(void* obj, DWORD now)
{
    for (int i = 0; i < (int)(sizeof(s_hl) / sizeof(s_hl[0])); i++) {
        if (s_hl[i].obj == obj && (now - s_hl[i].stamp) < 100) return true;
    }
    return false;
}

static void RememberHighlighted(void* obj, DWORD now)
{
    s_hl[s_hlNext].obj   = obj;
    s_hl[s_hlNext].stamp = now;
    s_hlNext = (s_hlNext + 1) % (int)(sizeof(s_hl) / sizeof(s_hl[0]));
}

static void __fastcall TextDrawDetour(void* self, void* unused)
{
    (void)unused;
    if (!self || !g_origTextDraw) return;

    char* obj = (char*)self;

    // Selection mode: the client draws a selectable textdraw twice per frame, the
    // second time with its highlight colour. Only the highlighted copy gets our
    // text, otherwise the same line appears twice in two colours; the plain pass
    // is still hidden, so the client's own glyphs stay invisible either way.
    const BYTE  hlFlag  = obj[samp::kOffHighlight];
    const DWORD hlColor = hlFlag ? *(DWORD*)(obj + samp::kOffHighlightColor) : 0;
    const DWORD nowT    = ::GetTickCount();
    if (hlFlag) RememberHighlighted(obj, nowT);
    const bool hlEarlier = !hlFlag && RecentlyHighlighted(obj, nowT);

    // Font gate: 0..3 are real text fonts; 4 is a sprite and 5 a 3D model
    // preview, and those never reach this branch (the dispatcher sends them
    // through the sprite renderer). The mask is configurable anyway.
    DWORD font = *(DWORD*)(obj + samp::kOffFont);
    if (font >= 32 || !(g_cfg.fontMask & (1u << font))) {
        CallOriginalTextDraw(self);
        return;
    }

    // Belt and braces: a sprite/preview slot means this is not plain text.
    if (*(DWORD*)(obj + samp::kOffSpriteIdx) != 0xFFFFFFFFu) {
        CallOriginalTextDraw(self);
        return;
    }

    // ~k~~PED_FIREWEAPON~ is expanded by the client's own function before drawing, so
    // the label follows the player's bindings. Do the same on a copy.
    char  kbuf[samp::kTextLen * 2];
    const char* text = ExpandKeys(obj + samp::kOffText, kbuf, sizeof(kbuf));

    // Length is bounded by the string itself: the rest of the buffer may hold
    // stale bytes, and converting them would append junk characters.
    int len = 0;
    bool need = false;
    for (; len < (int)samp::kTextLen - 1 && text[len]; len++) {
        if ((unsigned char)text[len] >= 0x80) need = true;
    }
    // [encoding] redraw_ascii = on takes the ASCII text too, so a station that mixes
    // English and CJK in one line does not switch fonts halfway through.
    if (!need && !g_cfg.redrawAscii) { CallOriginalTextDraw(self); return; }

    if (!hlFlag && hlEarlier) {
        HideTextAndCallOriginal(self, obj);
        return;
    }

    std::wstring w = ToWide(text, len);

    // The menu title arrives twice: CMenu draws it through its own encoder and the client also
    // hands the same string to a TextDraw. Both are visible, and the TextDraw copy sits where
    // the draw says - the title visibly jumps right for a frame while a menu opens. Matching
    // the menu's own title is what removes that copy from the very first frame: the client still
    // has to run (its text lengths drive the menu's own layout), so it is told to draw the copy
    // fully transparent instead of being skipped.
    if (g_cfg.menuEnabled && MenuWindowActive() && IsMenuTitleText(w)) {
        HideTextAndCallOriginal(self, obj);
        return;
    }

    if (w.empty()) { CallOriginalTextDraw(self); return; }

    // The client also renders the GameText through a TextDraw. We draw that string
    // ourselves with the style's own layout, so blank this copy (keeping its length
    // so the game's metrics do not move) and put the bytes back afterwards.
    if (IsGameTextText(w)) {
        // Both buffers: the renderer may read the copy at kOffText2, and the spaces
        // path below blanks them together for the same reason.
        char savedA[samp::kTextLen];
        char savedB[samp::kTextLen];
        if (len + 1 <= (int)samp::kTextLen) {
            ::memcpy(savedA, obj + samp::kOffText, (size_t)len + 1);
            ::memcpy(savedB, obj + samp::kOffText2, (size_t)len + 1);
            ::memset(obj + samp::kOffText, ' ', (size_t)len);
            ::memset(obj + samp::kOffText2, ' ', (size_t)len);
            CallOriginalTextDraw(self);
            ::memcpy(obj + samp::kOffText, savedA, (size_t)len + 1);
            ::memcpy(obj + samp::kOffText2, savedB, (size_t)len + 1);
            return;
        }
    }

    float letterW = *(float*)(obj + samp::kOffLetterW);
    float letterH = *(float*)(obj + samp::kOffLetterH);
    float bx      = *(float*)(obj + samp::kOffX);
    float by      = *(float*)(obj + samp::kOffY);
    int   sw      = *(int*)samp::kAddrScreenW;
    int   sh      = *(int*)samp::kAddrScreenH;

    bool valid = (letterH > 0.01f && letterH < 40.0f) &&
                 (letterW >= 0.0f && letterW < 40.0f) &&
                 (bx > -2000.0f && bx < 2000.0f) &&
                 (by > -2000.0f && by < 2000.0f) &&
                 sw > 0 && sh > 0;

    if (valid) {
        float baseX = bx * (float)sw / 640.0f;                       // x on the 640x448 virtual screen
        float baseY = by * (float)sh / 448.0f + g_cfg.baselineOffset;    // y on the 640x448 virtual screen + configured offset
        int   px    = (int)(letterH * (float)sh / g_cfg.sizeDivisor + 0.5f);
        if (px < 6)   px = 6;
        if (px > 200) px = 200;

        D3DCOLOR c1 = SwapRB(hlFlag ? hlColor : *(DWORD*)(obj + samp::kOffColor));
        D3DCOLOR c2 = SwapRB(*(DWORD*)(obj + samp::kOffColor2));
        // 0xFF is this field's "not set" sentinel and the client filters it *before* GTA sees it:
        // it compares the signed byte (0xFF = -1) and only calls Font_SetOutline for a positive
        // value. Passing 0xFF through as 255 put eight copies 1020 x 769 px away on screen - the
        // faint copies of every server draw - which is not what the client shows for the same
        // TextDraw. Reading signed and treating anything <= 0 as "no outline" is what the client
        // does; a positive value up to 127 is drawn, stretched like GTA stretches it.
        //
        // The cast has to be explicit: obj is a char*, and char is signed on MSVC.
        int  shadowSize  = (int)(signed char)obj[samp::kOffFlagShadow];
        int  outlineSize = (int)(signed char)obj[samp::kOffFlagOutline];
        if (shadowSize  < 0) shadowSize  = 0;
        if (outlineSize < 0) outlineSize = 0;
        if (!g_cfg.shadow) shadowSize = 0;
        bool center  = *(BYTE*)(obj + samp::kOffFlagCenter) != 0;
        bool right   = *(BYTE*)(obj + samp::kOffFlagRight) != 0;

        std::vector<TextRun> runs = ParseColorRuns(w, c1, g_cfg.braceColors);

        // ~n~ and literal newlines become separate lines; colours stay per run.
        struct Line { std::vector<TextRun> runs; };
        std::vector<Line> lines(1);
        for (auto& r : runs) {
            std::vector<std::wstring> parts = SplitLines(r.text);
            for (size_t k = 0; k < parts.size(); k++) {
                if (k > 0) lines.push_back(Line());
                if (!parts[k].empty()) lines.back().runs.push_back(TextRun{ parts[k], r.color });
            }
        }

        SetAtlasWeight(g_cfg.clientTextWeight);
    int   lineH = MeasureLineHeight(px, (int)font);
        float y     = baseY;

        // Measure first: one horizontal scale is derived from the whole string
        // and then applied to every line, so the text keeps the aspect ratio the
        // server asked for with letterWidth / letterHeight.
        //
        // Each colour run is split again by face: the TextDraw font id selects
        // one of the original renderer's faces, and characters that face has no
        // glyph for (CJK) fall back to the configured display font.
        struct Piece { std::wstring text; D3DCOLOR color; int font; int width; int symbol; };
        std::vector<std::vector<Piece>> pieces(lines.size());
        int totalAll = 0;
        for (size_t li = 0; li < lines.size(); li++) {
            for (size_t i = 0; i < lines[li].runs.size(); i++) {
                std::vector<DrawPiece> dp;
                SplitRunIntoPieces(lines[li].runs[i].text, px, (int)font, dp);
                for (size_t k = 0; k < dp.size(); k++) {
                    pieces[li].push_back(Piece{ dp[k].text, lines[li].runs[i].color,
                                                dp[k].font, dp[k].width, dp[k].symbol });
                    totalAll += dp[k].width;
                }
            }
        }
        // Text made only of colour codes (~r~, {RRGGBB}) produces no pieces at
        // all. Blanking it would leave an empty line, so hand the item back to the
        // game instead - it draws nothing either way, but keeps the box.
        bool anyPiece = false;
        for (size_t li = 0; li < pieces.size(); li++) if (!pieces[li].empty()) anyPiece = true;
        if (!anyPiece) valid = false;

        int symbolsPx = 0;
        for (size_t li = 0; li < pieces.size(); li++)
            for (size_t i = 0; i < pieces[li].size(); i++)
                if (pieces[li][i].symbol) symbolsPx += pieces[li][i].width;

        float sx = WidthScale(text, letterW, sw, (float)symbolsPx, totalAll);

        for (size_t li = 0; li < lines.size(); li++) {
            std::vector<Piece>& line = pieces[li];

            // The advance a symbol reserves is its own square, without the
            // letterWidth stretch the glyphs get, so the alignment has to sum the
            // same values the drawing loop advances by.
            float lineAdv = 0.0f;
            for (size_t i = 0; i < line.size(); i++)
                lineAdv += (float)line[i].width * (line[i].symbol ? 1.0f : sx);

            float x = baseX;
            if (right)       x -= lineAdv;
            else if (center) x -= lineAdv * 0.5f;

            // The client also feeds the menu title through a TextDraw. The menu path
            // already drew that string at this position and size, so skip this copy
            // instead of drawing the title twice.
            if (MenuWindowActive() && !line.empty() && IsMenuDuplicate(x, y, px)) {
                y += (float)lineH;
                continue;
            }

            for (size_t i = 0; i < line.size(); i++) {
                DrawItem it;
                it.text    = line[i].text;
                it.x       = x;
                it.y       = y;
                it.px      = px;
                it.fontId  = line[i].font;
                it.color   = line[i].color;
                // The drop colour is used exactly as the client uses it: textdraw.cpp:216 hands
                // backgroundColor to Font_SetDropColor and GTA draws both the outline and the
                // shadow in it, alpha included. Forcing the glyphs' alpha onto it turned the
                // client's 0x3F000000 outline into solid black, and the eight 255-unit offsets
                // (1020 px at 2560 wide) then covered the screen instead of fading away.
                it.color2      = c2;
                it.shadowColor = c2;
                it.shadowSize  = shadowSize;
                it.outlineSize = outlineSize;
                it.stretchOutline = true;   // the client's own field, in its 640x480 space
                it.isTextDraw = true;
                it.scaleX  = sx;
                if (line[i].symbol) {
                    // An inline arrow: the client's own sprite square, its own
                    // vertical offset, and no outline or shadow (the client returns
                    // right after drawing the sprite). The Unicode arrow stays in
                    // the text as the fallback for when the texture cannot be read.
                    it.sprite      = line[i].symbol;
                    it.text        = ArrowFallback(line[i].symbol);
                    it.px          = line[i].width;
                    it.y           = y + (float)SymbolOffsetPx(px);
                    it.outlineSize = 0;
                    it.shadowSize  = 0;
                }
                PushDrawItem(it);
                // The client advances a button sprite by its own 17H square, not by
                // the TextDrawLetterSize width, so the arrow keeps its aspect while
                // the letters around it stretch.
                x += (float)line[i].width * (line[i].symbol ? 1.0f : sx);
            }
            y += (float)lineH;
        }

        LogDbg(L"redraw TextDraw: font=%u px=%d '%s' (chars=%d, letterH=%.2f) at=(%.1f,%.1f) center=%d right=%d scale=%.1fx%.1f color=%08X color2=%08X boxFlag=%d boxColor=%08X outline=%d shadow=%d",
               font, px, w.c_str(), (int)w.size(), letterH,
               *(float*)(obj + samp::kOffX), *(float*)(obj + samp::kOffY),
               (int)center, (int)right,
               *(float*)(obj + samp::kOffBoxW), *(float*)(obj + samp::kOffBoxH),
               (unsigned)*(DWORD*)(obj + samp::kOffColor), (unsigned)*(DWORD*)(obj + samp::kOffColor2),
               (int)obj[samp::kOffBoxFlag], (unsigned)*(DWORD*)(obj + samp::kOffBoxColor),
               outlineSize, shadowSize);
    }

    // Only blank the source text when the item was actually queued. With
    // invalid geometry the source text is left untouched, so the player sees
    // mojibake instead of nothing at all.

    if (!valid) {
        CallOriginalTextDraw(self);
        return;
    }

    // Hide the game's own text while it does the layout, then restore it: the string
    // is kept and only the colour alphas are zeroed, so the game still measures the
    // real glyphs, draws the box that belongs to them, and keeps its own hit
    // rectangle for a selectable textdraw.
    HideTextAndCallOriginal(self, obj);
}

// ---------------------------------------------------------------------------
// The scoreboard
//
// SA-MP draws its scoreboard only while it is open, and the draw function starts
// with the frame setup below; the call site tests the board's own flag first:
//
//   mov  eax, [CScoreboard]     ; A1 imm32
//   cmp  dword ptr [eax], 0     ; 83 38 00
//   je   skip                   ; the panel is closed
//   ...
//   call CScoreboard::Draw
//
// The overlay draws after this call (at Present, or at EndScene, which is still
// later in the frame), so our GameText would be
// painted on top of the panel while the stock client draws its GameText during
// the HUD pass and the scoreboard covers it. Nothing here is tied to one client
// build: the draw function is found by its prologue, its caller by the relative
// call to it, and the flag by the compare right in front of that call.
// ---------------------------------------------------------------------------
static volatile DWORD* g_sbSlot   = nullptr;   // the global that holds CScoreboard*
static BYTE*           g_sbDraw   = nullptr;   // CScoreboard::Draw, hooked below
static bool            g_sbScanned = false;

static void FindScoreboardSlot()
{
    g_sbScanned = true;
    BYTE* base = nullptr; DWORD size = 0;
    if (!g_sampBase || !GetTextSection(g_sampBase, &base, &size)) return;

    static const BYTE kDrawPat[]  = { 0x83,0xEC,0x60,0x53,0x55,0x56,0x8B,0xF1,0x83,0x3E,0x00 };
    static const char kDrawMask[] = "xxxxxxxxxxx";
    BYTE* draw = FindPattern(base, size, kDrawPat, kDrawMask, 11);
    if (!draw) {
        LogW(L"scoreboard: draw function not found, the panel will stay uncovered");
        return;
    }
    g_sbDraw = draw;

    for (DWORD i = 0; i + 6 < size; i++) {
        if (base[i] != 0xE8) continue;
        BYTE* target = base + i + 5 + *(int*)(base + i + 1);
        if (target != draw) continue;

        // Walk back to the "cmp dword ptr [reg], 0" that guards the call, then to
        // the load of that same register from the scoreboard pointer global.
        for (int k = 3; k <= 48; k++) {
            if (i < (DWORD)k + 6) break;            // stay inside .text
            BYTE* q = base + i - k;
            if (q[0] != 0x83 || (q[1] & 0xF8) != 0x38 || q[2] != 0x00) continue;

            const BYTE cmpReg = q[1] & 7;                  // 0=eax 1=ecx 2=edx 3=ebx
            BYTE* slot = nullptr;
            if (q[-5] == 0xA1 && cmpReg == 0) {            // mov eax, imm32
                slot = *(BYTE**)(q - 4);
            } else if (q[-6] == 0x8B && (q[-5] & 0xC7) == 0x04 && ((q[-5] >> 3) & 7) == cmpReg) {
                slot = *(BYTE**)(q - 4);                   // mov reg, imm32 (modrm 05/0D/15/1D)
            }
            if (!slot) continue;

            __try {
                DWORD* p = (DWORD*)slot;
                DWORD  v = *p;                              // CScoreboard*, may still be null
                if (v) (void)*(volatile DWORD*)v;           // must be readable when set
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }

            g_sbSlot = (DWORD*)slot;
            LogW(L"scoreboard: flag at samp.dll+0x%X, the panel is read from it",
                 (unsigned)((BYTE*)slot - (BYTE*)g_sampBase));
            return;
        }
    }
    LogW(L"scoreboard: flag not found, the panel will stay uncovered");
}

bool ScoreboardUp()
{
    if (!g_sbScanned) FindScoreboardSlot();
    DWORD* slot = (DWORD*)g_sbSlot;
    if (!slot) return false;
    __try {
        DWORD* board = (DWORD*)*slot;
        return board != nullptr && *board != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// The client draws a chat line in pieces, each positioned with Arial's advance for everything
// before it: "[hh:mm:ss] ", then "Name:", then the message. Our face is narrower, so every
// following piece lands past our own end and a gap opens. Pulling it back to the previous
// piece's end closes that, but only for a piece that *continues a chat line*: the TAB
// scoreboard also draws its column headers as separate calls on one row, and joining those
// glued "id" to "name". So the previous piece has to look like one of the two chat prefixes -
// a timestamp bracket or a name with its colon - and not like a column header.
static bool LooksLikeChatPrefix(const std::wstring& s)
{
    size_t n = s.size();
    while (n > 0 && s[n - 1] == L' ') n--;
    if (n == 0) return false;
    const wchar_t last = s[n - 1];
    return last == L':' || last == L']';
}

// A chat line reaches this mod as up to three calls - the timestamp, the name and the message -
// each carrying an x the client measured with its own face. Ours has different advances, so a piece
// lands where Arial said it should rather than where the previous piece ended.
//
// This keeps the client's x for every piece and corrects it by the difference accumulated so far:
// the previous piece's width as this mod rendered it, minus the advance the client itself used
// between the two. Nothing is dropped, re-ordered or re-measured beyond that shift, and only the
// chat is touched - the scope flag decides, so a 3D label or a nametag never goes through it.
static float ChatOffsetX(long top, float clientX, const std::wstring& text, int px, int fontId)
{
    static long         s_top = 0;
    static float        s_shift = 0.0f;
    static float        s_prevClientX = 0.0f;
    static std::wstring s_prevText;
    static int          s_prevPx = 0;
    static int          s_prevFont = -1;
    static bool         s_havePrev = false;

    const bool newLine = !s_havePrev || top != s_top;
    if (newLine) {
        s_top = top;
        s_shift = 0.0f;
    } else if (!s_prevText.empty() && clientX > s_prevClientX) {
        // The client puts its next piece five pixels after what it drew: the timestamp starts at 45
        // and the message at m_nTimestampWidth + 50, an advance of the timestamp width plus five.
        // Since the shift only corrects for the width difference, that five cancelled out and the
        // message ended up flush against the timestamp. Keep it as a floor instead: at least five
        // pixels after this mod's own glyphs, and never closer than the client would have put it.
        const float clientAdvance = clientX - s_prevClientX;
        const int   ours = MeasureTextWidth(s_prevText, s_prevPx, s_prevFont);
        if (ours > 0) {
            const float kPieceGap = 5.0f;
            float advance = (float)ours + kPieceGap;
            if (clientAdvance > advance) advance = clientAdvance;
            s_shift += advance - clientAdvance;
        }
    }

    const float x = clientX + s_shift;
    s_prevClientX = clientX;
    s_prevText    = text;
    s_prevPx      = px;
    s_prevFont    = fontId;
    s_havePrev    = true;
    return x;
}// Which hook is drawing, so a doubled line can be traced back to its two sources.
static const wchar_t* g_drawSource = L"?";

static void TakeOverClientText(int which, void* self, void* sprite, const char* text,
                               long l, long t, long r, long b, DWORD colour, long format,
                               int forcePx, const wchar_t* wideText);

// ---------------------------------------------------------------------------
// Text that belongs *inside* a panel has to be painted after the panel, or the
// translucent fill covers it - that is why the rows and the OK label disappeared when
// they were drawn where the client draws them. So a call made while the panel draws is
// recorded and painted once its draw has finished.
// ---------------------------------------------------------------------------
struct PendingText {
    std::wstring text;
    long   l, t, r, b;
    DWORD  colour;
    long   format;
    int    px;
    // Carried with the line because it is painted later, when the per-line overrides have
    // already been reset: without these a dialog's caption came out at the default weight
    // and face instead of the ones the client asked for.
    int    weight;
    int    fontId;
    float  scaleX;
    int    copies;
};

static std::vector<PendingText> g_pending;
static CRITICAL_SECTION         g_pendingCs;
static std::once_flag           g_pendingOnce;
static volatile LONG            g_pendingReady = 0;
static volatile LONG            g_inPanelDraw = 0;
static volatile LONG            g_wrapperChatCall = 0;

// Per-line style: set by whoever knows it for the duration of one TakeOverClientText call,
// and carried through the deferred queue as well (see PendingText).
static int    g_shadowCopies  = -1;     // -1 auto, else 0 none / 4 cross / 8 wide pattern
static volatile LONG g_weightOverride = -1;  // -1 auto, else the rasterisation weight
static int    g_fontIdOverride = -1;    // -1 auto, else the face for this line
static float  g_hscaleOverride = -1.0f; // < 0 auto, else the horizontal scale
static volatile DWORD           g_wrapperRetRva = 0;   // caller of the current wrapper call   // this call came from CChat::Draw

static void PushPendingText(const PendingText& item)
{
    std::call_once(g_pendingOnce, []() { ::InitializeCriticalSection(&g_pendingCs); });
    g_pendingReady = true;
    ::EnterCriticalSection(&g_pendingCs);
    if (g_pending.size() < 128) g_pending.push_back(item);
    ::LeaveCriticalSection(&g_pendingCs);
}

static void FlushPendingText()
{
    static bool s_init = false;
    static std::once_flag s_once;
    (void)s_once;
    if (!g_pendingReady) return;
    std::vector<PendingText> items;
    ::EnterCriticalSection(&g_pendingCs);
    items.swap(g_pending);
    ::LeaveCriticalSection(&g_pendingCs);
    if (items.empty()) return;

    for (size_t i = 0; i < items.size(); i++) {
        const int prevWeight = (int)g_weightOverride;
        const int prevFace   = g_fontIdOverride;
        const float prevHs   = g_hscaleOverride;
        const int prevCopies = g_shadowCopies;
        g_weightOverride  = items[i].weight;
        g_fontIdOverride  = items[i].fontId;
        g_hscaleOverride  = items[i].scaleX;
        g_shadowCopies    = items[i].copies;
        { const wchar_t* prevSrc = g_drawSource; g_drawSource = L"pending";
        TakeOverClientText(0, (void*)nullptr, nullptr, nullptr, items[i].l, items[i].t,
                           items[i].r, items[i].b, items[i].colour, items[i].format,
                           items[i].px, items[i].text.c_str());
        g_drawSource = prevSrc; }
        g_weightOverride = prevWeight;
        g_fontIdOverride = prevFace;
        g_hscaleOverride = prevHs;
        g_shadowCopies   = prevCopies;
    }
}



// ---------------------------------------------------------------------------
// CDXUTDialog::OnRender (0.3.DL samp.dll+0x90360).

extern std::atomic<bool> g_inReset;
//
// The trouble with this function is what is *inside* it: it opens
//   m_pManager->m_pSprite->Begin(D3DXSPRITE_DONOTSAVESTATE);
// draws the dialog background, every control and its caption, and closes with
//   m_pManager->m_pSprite->End();  m_pManager->m_pStateBlock->Apply();
// Any drawing done while that batch is open corrupts it - two attempts to hook the entry
// point and paint the queued captions crashed the game exactly there.
//
// So the entry point is left alone. Instead every direct `call` to it is redirected to a
// stub of the same shape, which runs the real function first and only then paints: at that
// point the sprite batch has been closed and the state block applied, and the caller's own
// frame code is outside it. Seven such call sites exist in 0.3.DL, one per dialog.
// ---------------------------------------------------------------------------
typedef HRESULT(__fastcall* DlgRenderFn)(void* self, void* unused, float elapsed);
static DlgRenderFn g_origDlgRender = nullptr;
static int         g_dlgCallSites = 0;

static HRESULT __fastcall DlgRenderStub(void* self, void* unused, float elapsed);

static BYTE* AddrOfDlgRenderStub()
{
    union { HRESULT(__fastcall* f)(void*, void*, float); BYTE* p; } u;
    u.f = &DlgRenderStub;
    return u.p;
}

static HRESULT __fastcall DlgRenderStub(void* self, void* unused, float elapsed)
{
    (void)unused;
    // The whole function draws inside an open sprite batch, so nothing may be painted
    // while it runs: the captions are queued (this is the flag every other panel hook
    // uses) and painted once it has returned, with the batch closed.
    ::InterlockedExchange(&g_inPanelDraw, 1);
    HRESULT hr = g_origDlgRender ? g_origDlgRender(self, nullptr, elapsed) : E_FAIL;
    ::InterlockedExchange(&g_inPanelDraw, 0);

    __try {
        if (!g_inReset.load() && RenderDevice()) FlushPendingText();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A failed flush costs the frame's captions, never the process.
    }
    return hr;
}

// ---------------------------------------------------------------------------
// CChat::Render: the one function the client draws chat lines from
//
// Whether a text call belongs to the chat is the input to every chat decision - the pen, the
// duplicate filter, the shadow pass - and it used to be inferred twice: from a caller-address
// window measured per build, and from a table filled in at runtime by matching a call's text
// against the last shadow copy. The second is unsound, because the same string is drawn by a
// nametag, a 3D label and the chat alike, so a label's call site could be recorded as a chat one
// and then have the chat's pen and duplicate filter applied to it.
//
// The scope turns it into a fact. CChat::Render is found by its prologue, which occurs exactly
// once in .text of every client this mod ships for:
//
//   0.3.7-R1 0x638A0   0.3.7-R2 0x63970   0.3.7-R3 and R3-1 0x66CF0
//   0.3.7-R4 0x67430   0.3.7-R5 0x67470   0.3.DL 0x66EE0
//
// and its call sites are redirected to a stub that raises a flag for the duration of the call.
// Inside it text is chat; outside it text is not, whatever it says.
static volatile LONG g_inChatRender = 0;
static bool          g_chatRenderHooked = false;

// CChat::Render takes six dwords on the stack and ends with `ret 18h`, so the stub has to accept
// them and hand the same six back: a stub that pops nothing leaves the caller's stack 24 bytes out
// of place, which crashes on the first return.
typedef void(__fastcall* ChatRenderFn)(void* self, void* unused, DWORD a1, DWORD a2, DWORD a3,
                                       DWORD a4, DWORD a5, DWORD a6);
static ChatRenderFn g_origChatRender = nullptr;

static void __fastcall ChatRenderStub(void* self, void* unused, DWORD a1, DWORD a2, DWORD a3,
                                      DWORD a4, DWORD a5, DWORD a6);

// True only while CChat::Render is on the stack. When the prologue was not found this returns
// false for everything, which leaves the per-build caller windows in charge exactly as before.
static bool InChatRender() { return g_chatRenderHooked && g_inChatRender != 0; }

// Is this call part of the chat? Once CChat::Render is scoped, that scope is the only answer:
// the per-build caller window and the seeded site table are inferences, and an inference that
// can be wrong is only worth keeping for a client whose prologue was not found.
static bool ChatPiece() { return g_chatRenderHooked ? InChatRender() : g_wrapperChatCall != 0; }

static void __fastcall ChatRenderStub(void* self, void* unused, DWORD a1, DWORD a2, DWORD a3,
                                      DWORD a4, DWORD a5, DWORD a6)
{
    ::InterlockedExchange(&g_inChatRender, 1);
    if (g_origChatRender) g_origChatRender(self, nullptr, a1, a2, a3, a4, a5, a6);
    ::InterlockedExchange(&g_inChatRender, 0);
}

bool HookChatRender()
{
    if (g_chatRenderHooked) return true;
    if (!g_sampBase) return false;

    BYTE* text = nullptr;
    DWORD textSize = 0;
    if (!GetTextSection((HMODULE)g_sampBase, &text, &textSize)) return false;

    // push ebp / mov ebp,esp / and esp,-8 / sub esp,20Ch / push ebx / mov ebx,ecx
    static const BYTE kSig[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8,
                                 0x81, 0xEC, 0x0C, 0x02, 0x00, 0x00,
                                 0x53, 0x8B, 0xD9 };
    static const char* kMask = "xxxxxxxxxxxxxxx";
    BYTE* target = FindPattern(text, textSize, kSig, kMask, (int)sizeof(kSig));
    if (!target) {
        LogW(L"CChat::Render: prologue not found, the chat keeps the per-build caller window");
        return false;
    }

    g_origChatRender = (ChatRenderFn)target;
    int patched = 0;
    for (BYTE* p = text; p + 5 <= text + textSize; p++) {
        if (*p != 0xE8) continue;
        const int rel = *(int*)(p + 1);
        if (p + 5 + rel != target) continue;
        ptrdiff_t delta = (BYTE*)&ChatRenderStub - (p + 5);
        if (delta < INT_MIN || delta > INT_MAX) continue;
        DWORD old = 0;
        if (!::VirtualProtect(p + 1, sizeof(int), PAGE_READWRITE, &old)) continue;
        FreezeThreads();
        *(int*)(p + 1) = (int)delta;
        UnfreezeThreads();
        DWORD tmp = 0;
        ::VirtualProtect(p + 1, sizeof(int), old, &tmp);
        patched++;
    }
    if (!patched) {
        LogW(L"CChat::Render: no call site found, the chat keeps the per-build caller window");
        return false;
    }

    g_chatRenderHooked = true;
    LogW(L"CChat::Render scoped at +0x%X, %d call site(s): chat is whatever runs inside it",
         (unsigned)(target - (BYTE*)g_sampBase), patched);
    return true;
}

bool HookDialogOnRender()
{
    if (g_dlgCallSites || !g_cfg.clientText || !g_sampBase) return g_dlgCallSites != 0;

    BYTE* base = (BYTE*)g_sampBase;

    BYTE* text = nullptr; DWORD textSize = 0;
    if (!GetTextSection((HMODULE)g_sampBase, &text, &textSize)) return false;

    // CDXUTDialog::OnRender is found by its prologue instead of a per-build offset:
    //   sub esp,0x180 / push edi / mov edi,ecx / fld qword ptr [edi+7]
    // Checked against every client that ships under the launcher's samp folder - 0.3.7-R1
    // (0x8C280), R2 (0x8C320), R3 and R3-1 (0x901D0), R4 (0x90910), R5 (0x908E0) and 0.3.DL
    // (0x90360) - where it matches exactly once each. The old hard-coded +0x90360 only held
    // for 0.3.DL, which is why 0.3.7 logged "no call site for CDXUTDialog::OnRender" and the
    // dialog captions were not painted after the batch closed.
    static const BYTE kOnRenderSig[] = { 0x81, 0xEC, 0x80, 0x01, 0x00, 0x00,
                                         0x57, 0x8B, 0xF9, 0xDD, 0x47, 0x07 };
    const char* kOnRenderMask = "xxxxxxxxxxxx";
    BYTE* target = FindPattern(text, textSize, kOnRenderSig, kOnRenderMask,
                               (int)sizeof(kOnRenderSig));
    if (!target) {
        LogW(L"CDXUTDialog::OnRender: prologue not found, captions keep the client's order");
        return false;
    }
    {
        // Two matches would mean the pattern is not specific enough; patching both would
        // redirect a function this mod knows nothing about.
        const DWORD used = (DWORD)(target + 1 - text);
        if (FindPattern(target + 1, textSize - used, kOnRenderSig, kOnRenderMask,
                        (int)sizeof(kOnRenderSig))) {
            LogW(L"CDXUTDialog::OnRender: prologue is ambiguous, hook skipped");
            return false;
        }
        LogW(L"CDXUTDialog::OnRender found by signature at +0x%X", (unsigned)(target - base));
    }
    BYTE* stub   = AddrOfDlgRenderStub();

    int patched = 0;
    for (BYTE* p = text; p + 5 <= text + textSize; p++) {
        if (*p != 0xE8) continue;                       // call rel32
        int rel = *(int*)(p + 1);
        if (p + 5 + rel != target) continue;

        // The stub is called exactly like the function it replaces, so only the operand
        // moves: the caller's return address and its stack arguments are untouched.
        ptrdiff_t delta = stub - (p + 5);
        if (delta < INT_MIN || delta > INT_MAX) continue;

        DWORD old = 0;
        if (!::VirtualProtect(p + 1, sizeof(int), PAGE_READWRITE, &old)) continue;
        FreezeThreads();
        *(int*)(p + 1) = (int)delta;
        UnfreezeThreads();
        DWORD tmp = 0;
        ::VirtualProtect(p + 1, sizeof(int), old, &tmp);
        patched++;
    }

    if (!patched) {
        LogW(L"no call site for CDXUTDialog::OnRender was found");
        return false;
    }

    g_origDlgRender = (DlgRenderFn)target;
    g_dlgCallSites = patched;
    LogW(L"dialog render: %d call site(s) redirected, captions are painted after the batch closes", patched);
    return true;
}

// ---------------------------------------------------------------------------
// CDialog::Draw (0.3.DL samp.dll+0x6F2D0; twelve byte prologue, ends on an instruction
// boundary):
//   83 EC 14        sub esp, 0x14
//   53 55 56 57     push ebx / ebp / esi / edi
//   8B F9           mov edi, ecx
//   8B 47 28        mov eax, [edi+0x28]
//
// The dialog paints its translucent fill inside that call, so a line drawn while it runs
// ends up under the fill - which is why its body looked grey while the scoreboard, whose
// fill comes first, was right. The same panel window the scoreboard uses is opened here,
// and the queued text is painted once the call has returned.
// ---------------------------------------------------------------------------
static const BYTE kDialogPat[12] = {
    0x83, 0xEC, 0x14, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x28,
};
static const char kDialogMask[12] = {
    'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x',
};

typedef void(__fastcall* DialogDrawFn)(void* self, void* unused);
static DialogDrawFn g_origDialogDraw = nullptr;
static bool          g_dialogHooked = false;

static void __fastcall DialogDrawDetour(void* self, void* unused);

static BYTE* AddrOfDialogDetour()
{
    union { void(__fastcall* f)(void*, void*); BYTE* p; } u;
    u.f = &DialogDrawDetour;
    return u.p;
}

static void __fastcall DialogDrawDetour(void* self, void* unused)
{
    (void)unused;
    ::InterlockedExchange(&g_inPanelDraw, 1);
    if (g_origDialogDraw) g_origDialogDraw(self, nullptr);
    ::InterlockedExchange(&g_inPanelDraw, 0);
    FlushPendingText();
}

bool HookDialogDraw()
{
    if (g_dialogHooked || !g_cfg.clientText || !g_sampBase) return g_dialogHooked;

    BYTE* base = nullptr; DWORD size = 0;
    if (!GetTextSection((HMODULE)g_sampBase, &base, &size)) return false;

    BYTE* target = FindPattern(base, size, kDialogPat, kDialogMask, 12);
    if (!target) {
        LogW(L"signature for CDialog::Draw not found");
        return false;
    }

    const int kLen = 12;
    BYTE* tramp = (BYTE*)::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    ::memcpy(tramp, target, kLen);
    tramp[kLen] = 0xE9;
    if (!FitsRel32(tramp + kLen, target + kLen, L"dialog trampoline")) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    *(int*)(tramp + kLen + 1) = (int)((target + kLen) - (tramp + kLen + 5));

    if (!FitsRel32(target, AddrOfDialogDetour(), L"dialog detour")) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    BYTE patch[16];
    patch[0] = 0xE9;
    *(int*)(patch + 1) = (int)(AddrOfDialogDetour() - (target + 5));
    for (int i = 5; i < kLen; i++) patch[i] = 0x90;

    DWORD old = 0;
    if (!::VirtualProtect(target, kLen, PAGE_EXECUTE_READWRITE, &old)) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    FreezeThreads();
    ::memcpy(target, patch, kLen);
    UnfreezeThreads();
    DWORD tmp = 0;
    ::VirtualProtect(target, kLen, old, &tmp);

    g_origDialogDraw = (DialogDrawFn)tramp;
    g_dialogHooked = true;
    LogW(L"dialog draw hooked: its text is painted after its fill");
    return true;
}

// ---------------------------------------------------------------------------
// Scoreboard draw: paint the GameText layer before the panel
//
// The panel is translucent, so the client's own GameText is visible through it.
// Our overlay draws after the panel (Present, or EndScene - both are later), which
// is why the layer used
// to be skipped entirely while the panel was open. Hooking the panel's draw puts
// our quads down first, so the panel is drawn over them and the text shows
// through it exactly like the client's own does.
//
// The prologue is eleven bytes and ends on an instruction boundary:
//   83 EC 60    sub esp, 0x60
//   53 55 56    push ebx / ebp / esi
//   8B F1       mov esi, ecx
//   83 3E 00    cmp dword ptr [esi], 0
// ---------------------------------------------------------------------------
typedef void(__fastcall* ScoreboardDrawFn)(void* self, void* unused);
static ScoreboardDrawFn g_origScoreboardDraw = nullptr;
static bool              g_sbHooked = false;

static void __fastcall ScoreboardDrawDetour(void* self, void* unused);

static BYTE* AddrOfSbDetour()
{
    union { void(__fastcall* f)(void*, void*); BYTE* p; } u;
    u.f = &ScoreboardDrawDetour;
    return u.p;
}

static void __fastcall ScoreboardDrawDetour(void* self, void* unused)
{
    (void)unused;
    if (g_cfg.scoreboardUnder) DrawUnderPanel(RenderDevice());

    ::InterlockedExchange(&g_inPanelDraw, 1);
    if (g_origScoreboardDraw) g_origScoreboardDraw(self, nullptr);
    ::InterlockedExchange(&g_inPanelDraw, 0);

    // The panel and its own text are on screen now: the lines recorded during that
    // draw go on top of the fill, where the client puts them too.
    FlushPendingText();
}

bool HookScoreboardDraw()
{
    if (!g_cfg.scoreboardUnder) return false;
    if (!g_sbScanned) FindScoreboardSlot();      // also fills g_sbDraw
    if (!g_sbDraw) return false;
    if (g_sbHooked) return true;

    const int kLen = 11;
    BYTE* p = g_sbDraw;

    BYTE* tramp = (BYTE*)::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    ::memcpy(tramp, p, kLen);
    tramp[kLen] = 0xE9;
    if (!FitsRel32(tramp + kLen, p + kLen, L"scoreboard trampoline")) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    *(int*)(tramp + kLen + 1) = (int)((p + kLen) - (tramp + kLen + 5));
    g_origScoreboardDraw = (ScoreboardDrawFn)tramp;

    BYTE patch[16];
    patch[0] = 0xE9;
    if (!FitsRel32(p, (const void*)AddrOfSbDetour(), L"scoreboard detour")) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        g_origScoreboardDraw = nullptr;
        return false;
    }
    *(int*)(patch + 1) = (int)(AddrOfSbDetour() - (p + 5));
    for (int i = 5; i < kLen; i++) patch[i] = 0x90;

    FreezeThreads();
    bool ok = WriteMemory(p, patch, kLen);
    UnfreezeThreads();
    if (!ok) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        g_origScoreboardDraw = nullptr;
        return false;
    }

    LogW(L"scoreboard: draw hooked, the GameText layer goes under the panel");
    g_sbHooked = true;
    return true;
}

bool HookTextDraw()
{
    if (!g_sampBase) return false;
    BYTE* base = nullptr; DWORD size = 0;
    if (!GetTextSection(g_sampBase, &base, &size)) return false;

    BYTE* thunk = FindPattern(base, size, kThunkPat, kThunkMask, 19);
    if (!thunk) {
        LogW(L"signature for CTextDraw::Draw not found");
        return false;
    }

    g_tdThunk = thunk;
    BYTE* realAddr = thunk + samp::kThunkTextJmpEnd + *(int*)(thunk + samp::kThunkTextJmpOp);

    // Sanity check: the real function must live inside .text, otherwise this
    // was a false positive.
    if (realAddr < base || realAddr >= base + size) {
        LogW(L"signature matched but the jump target is outside .text, aborting");
        return false;
    }
    g_origTextDraw = (TextDrawFn)realAddr;

    DWORD oldRel = *(DWORD*)(thunk + samp::kThunkTextJmpOp);
    g_tdOrigRel32 = oldRel;

    // The operand is an int: if the detour sits more than 2 GB away the cast wraps
    // and the jmp silently lands somewhere else.
    if (!FitsRel32(thunk + samp::kThunkTextJmpOp, (const void*)AddrOfTdDetour(),
                   L"TextDraw detour")) {
        return false;
    }

    FreezeThreads();
    DWORD newRel = (DWORD)(AddrOfTdDetour() - (thunk + samp::kThunkTextJmpEnd));
    bool ok = WriteMemory(thunk + samp::kThunkTextJmpOp, &newRel, 4);
    UnfreezeThreads();

    if (!ok) { LogW(L"patching failed (VirtualProtect)"); return false; }

    LogW(L"target: samp.dll signature matched, CTextDraw::Draw RVA=0x%X (text renderer 0x%X)",
         (unsigned)(thunk - (BYTE*)g_sampBase), (unsigned)(realAddr - (BYTE*)g_sampBase));
    LogW(L"TextDraw hook installed");
    g_tdHooked = true;
    return true;
}

// ---------------------------------------------------------------------------
// GameText
// ---------------------------------------------------------------------------
typedef void(__stdcall* GameTextFn)(char* text, int time, int style);
static GameTextFn g_origGameText = nullptr;   // trampoline
static BYTE*      g_gtTarget = nullptr;
static BYTE       g_gtOrigBytes[samp::kGameTextProlog] = { 0 };
static bool       g_gtHooked = false;

static const BYTE kGtPat[10] = { 0x55, 0x8B, 0xEC, 0x81, 0x7D, 0x10, 0xC8, 0x00, 0x00, 0x00 };
static const char kGtMask[10] = { 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x' };

static void __stdcall GameTextDetour(char* text, int time, int style)
{
    if (!g_origGameText) return;
    if (!text || !g_cfg.gameTextEnabled) { g_origGameText(text, time, style); return; }

    // Style gate: [gametext] ignore_styles lists the styles that keep the
    // game's own renderer (empty by default).
    if (style >= 0 && style < 32 && (g_cfg.gameTextIgnore & (1u << style))) {
        g_origGameText(text, time, style);
        return;
    }

    // 0..6 are the stock styles, and each has a fixed layout in render.cpp.
    // 7..15 only exist in fixes.inc / GameText+, where the server draws them
    // with textdraws - a raw call with those numbers keeps the game renderer.
    if (style < 0 || style > 6) {
        g_origGameText(text, time, style);
        return;
    }

    // Any new message for this style replaces the one we are showing, even when
    // the new text is ASCII - or empty, which is how HideGameTextForPlayer
    // clears a style - and therefore goes back to the game renderer. Without
    // this an English message would leave our Chinese one on screen.
    ClearGameTextAll();   // one GameText slot, like the client

    // ~k~~KEY~ is expanded by the game before it draws; our path has to do the same,
    // otherwise the label of the current binding is missing.
    char  kbuf[0x200 * 2];
    text = (char*)ExpandKeys(text, kbuf, sizeof(kbuf));

    // The packet buffer is only guaranteed up to the terminating NUL.
    int len = 0;
    bool need = false;
    for (; len < 0x200 - 1 && text[len]; len++) {
        if ((unsigned char)text[len] >= 0x80) need = true;
    }
    if (!need && !g_cfg.redrawAscii) { g_origGameText(text, time, style); return; }

    std::wstring w = ToWide(text, len);
    if (w.empty()) { g_origGameText(text, time, style); return; }

    // The time is passed through untouched for every style: the client's own
    // clamps (style 0 -> 9 s, style 1 -> 8 s fade loop, style 5 -> 3 s plus a
    // spam gate) are the bug the server-side fixes remove, so they are not
    // reproduced here. 0 means "until replaced", the same as in fixes.inc.
    PushGameText(w, style, time);
    LogDbg(L"redraw GameText: style=%d time=%d '%s'", style, time, w.c_str());

    // Feed the game function an equally long run of spaces so its queue is
    // replaced (nothing is drawn twice) while nothing becomes visible.
    //
    // Style 0 must never reach it: the client treats style 0 as the "big
    // message" and blanks every TextDraw and every other GameText for nine
    // seconds. That is exactly the behaviour the fixes drop, so the blanking
    // call uses a normal style instead.
    char blank[0x218];
    int n = 0;
    while (text[n] && n < 0x1FF) { blank[n] = ' '; n++; }
    blank[n] = 0;

    g_origGameText(blank, time, (style == 0) ? 1 : style);
}

bool HookGameText()
{
    if (!g_sampBase) return false;
    BYTE* base = nullptr; DWORD size = 0;
    if (!GetTextSection(g_sampBase, &base, &size)) return false;

    BYTE* p = FindPattern(base, size, kGtPat, kGtMask, 10);
    if (!p) { LogW(L"signature for CGame::DisplayGameText not found"); return false; }

    g_gtTarget = p;
    const int kLen = samp::kGameTextProlog;

    BYTE* tramp = (BYTE*)::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;

    ::memcpy(tramp, p, kLen);
    tramp[kLen] = 0xE9;
    if (!FitsRel32(tramp + kLen, p + kLen, L"trampoline jump back")) { ::VirtualFree(tramp, 0, MEM_RELEASE); return false; }
    *(int*)(tramp + kLen + 1) = (int)((p + kLen) - (tramp + kLen + 5));
    g_origGameText = (GameTextFn)tramp;

    ::memcpy(g_gtOrigBytes, p, kLen);

    BYTE patch[16];
    patch[0] = 0xE9;
    if (!FitsRel32(p, (const void*)AddrOfGtDetour(), L"GameText detour")) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        g_origGameText = nullptr;
        return false;
    }
    *(int*)(patch + 1) = (int)(AddrOfGtDetour() - (p + 5));
    for (int i = 5; i < kLen; i++) patch[i] = 0x90;

    FreezeThreads();
    bool ok = WriteMemory(p, patch, kLen);
    UnfreezeThreads();
    if (!ok) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        g_origGameText = nullptr;
        return false;
    }

    LogW(L"target: CGame::DisplayGameText RVA=0x%X", (unsigned)(p - (BYTE*)g_sampBase));
    LogW(L"GameText hook installed");
    g_gtHooked = true;
    return true;
}

// ---------------------------------------------------------------------------
// Native menus (GTA CMenu)
//
// CMenu::Show draws no text itself: it pushes every line through one encoder,
// and that encoder writes a small display list instead of calling the text
// renderer with a string:
//
//   0                    end of list
//   1 + dword            integer argument
//   3 + word             width-table entry (per-character widths are written back)
//   6 + float            position / scale argument
//   0xE + len + bytes    string - the menu text, in the server's encoding
//
// The string bytes are therefore available verbatim, and a same-length run of
// spaces keeps the layout and the width table valid. This step only dumps the
// list: which float is x, y or the scale has to be confirmed on a real menu
// before anything is replaced and drawn.
// ---------------------------------------------------------------------------
typedef int(__cdecl* MenuEncoderFn)(void);
static MenuEncoderFn g_origMenuEncoder = nullptr;
static BYTE*          g_menuTarget = nullptr;
static BYTE           g_menuOrigBytes[5] = { 0 };
static bool           g_menuHooked = false;
static BYTE*          g_menuBuf = nullptr;   // display list (absolute address)
static DWORD*         g_menuObj = nullptr;   // pointer to the state object

static const BYTE kMenuPat[32] = {
    0xB8, 0x00, 0x00, 0x00, 0x00,              // mov eax, <display list>
    0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00,        // mov ecx, [<object>]
    0x89, 0x41, 0x14,                          // mov [ecx+0x14], eax
    0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,        // call [<renderer>]
    0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00,        // mov ecx, [<object>]
    0x8B, 0x81, 0xC5, 0x00, 0x00, 0x00,        // mov eax, [ecx+0xC5]
};

static const char kMenuMask[32] = {
    'x', '?', '?', '?', '?',
    'x', 'x', '?', '?', '?', '?',
    'x', 'x', 'x',
    'x', 'x', '?', '?', '?', '?',
    'x', 'x', '?', '?', '?', '?',
    'x', 'x', 'x', 'x', 'x', 'x',
};

static void*         g_menuSelf = nullptr;   // CMenu last passed to CMenu::Show
// The rectangle CMenu::Show laid the panel out in, in screen pixels. A row is only taken
// when its pen falls inside this, so the HUD text the game keeps drawing while the menu is
// up (money top right, clock and radio at the bottom) can never be caught.
static RECT          g_menuPanel = { -1, -1, -1, -1 };
static volatile DWORD g_menuUntil = 0;       // tick until which panel text counts as menu text
static volatile LONG g_menuShowDepth = 0;
static volatile LONG g_menuCallsInShow = 0;
static int           g_menuShowDumps = 0;

static int __cdecl MenuEncoderDetour(void)
{
    if (!g_origMenuEncoder) return 0;      // hook installed without a trampoline
    if (g_menuShowDepth) {
        g_menuCallsInShow++;
    }
    return g_origMenuEncoder();
}

// CMenu::Show is the menu render entry. Watching it tells us whether a menu
// reached the client at all, and whether the encoder below is used for it.
typedef void(__fastcall* MenuShowFn)(void* self, void* unused);
static MenuShowFn g_origMenuShow = nullptr;
static BYTE*      g_menuShowTarget = nullptr;
static BYTE       g_menuShowOrigBytes[samp::kMenuShowProlog] = { 0 };
static bool       g_menuShowHooked = false;

static const BYTE kShowPat[12] = {
    0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x01, 0x84, 0xC0, 0x57,
};
static const char kShowMask[12] = {
    'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x',
};

// packed CMenu: x +0x361, y +0x365, widths +0x369/+0x36D, columns +0x371
static void MenuComputePanel(void* self)
{
    if (!self) return;
    const BYTE* b = (const BYTE*)self;
    const float mx   = *(const float*)(b + 0x361);
    const float my   = *(const float*)(b + 0x365);
    const float w0   = *(const float*)(b + 0x369);
    const float w1   = *(const float*)(b + 0x36D);
    const BYTE  cols = b[0x371];
    const int   sw   = *(const int*)samp::kAddrScreenW;
    const int   sh   = *(const int*)samp::kAddrScreenH;
    if (sw <= 0 || sh <= 0) return;
    if (!(mx > -100.0f && mx < 800.0f && my > -100.0f && my < 600.0f)) return;
    const float kx = (float)sw / 640.0f, ky = (float)sh / 448.0f;
    const float w  = w0 + (cols > 1 ? w1 : 0.0f);
    g_menuPanel.left   = (LONG)((mx - 10.0f) * kx);
    g_menuPanel.top    = (LONG)((my - 24.0f) * ky);
    g_menuPanel.right  = (LONG)((mx + w + 14.0f) * kx);
    g_menuPanel.bottom = (LONG)((my + 280.0f) * ky);
}

// The title also reaches the screen as a TextDraw. Note its text so that copy is dropped from
// the first frame, before any position note exists.
static void MenuNoteTitle(void* self)
{
    if (!self) return;
    const char* title = (const char*)self + 1;      // CMenu::m_szTitle
    const int n = (int)::strnlen(title, 32);
    if (n <= 0) return;
    NoteMenuTitle(ToWide(title, n));
}

static void __fastcall MenuShowDetour(void* self, void* unused)
{
    if (!g_origMenuShow) return;           // hook installed without a trampoline
    g_menuSelf = self;                     // the CMenu the game is drawing now
    g_menuUntil = ::GetTickCount() + 3000;

    // The panel rectangle has to be known *before* the original runs: Show() is what draws the
    // title and the rows, so computing it afterwards left the first frame of every menu with a
    // stale (or unset) rectangle and its rows were rejected.
    MenuComputePanel(self);

    // The title is drawn twice: once through the menu's own encoder and once as a TextDraw.
    // The position note that suppresses the second copy only exists after the first frame, so
    // register the title now and the TextDraw copy is skipped from the very first frame.
    MenuNoteTitle(self);

    ::InterlockedIncrement(&g_menuShowDepth);
    LONG before = g_menuCallsInShow;
    g_origMenuShow(self, unused);
    ::InterlockedDecrement(&g_menuShowDepth);

    if (g_cfg.menuEnabled) {
        wchar_t hex[48];
        const BYTE* t = (const BYTE*)self + 1;      // CMenu::m_szTitle
        hex[0] = 0;
        for (int k = 0; k < 12; k++) {
            wchar_t one[8];
            swprintf_s(one, L"%02X", t[k]);
            wcscat_s(hex, one);
        }
        LogDbg(L"menu shown: title=%s encoderCalls=%ld",
             hex, (long)(g_menuCallsInShow - before));
    }
}

bool HookMenuShow()
{
    if (!g_sampBase) return false;
    BYTE* base = nullptr; DWORD size = 0;
    if (!GetTextSection(g_sampBase, &base, &size)) return false;

    BYTE* p = FindPattern(base, size, kShowPat, kShowMask, 12);
    if (!p) { LogW(L"signature for CMenu::Show not found"); return false; }

    const int kLen = samp::kMenuShowProlog;
    BYTE* tramp = (BYTE*)::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    ::memcpy(tramp, p, kLen);
    tramp[kLen] = 0xE9;
    if (!FitsRel32(tramp + kLen, p + kLen, L"trampoline jump back")) { ::VirtualFree(tramp, 0, MEM_RELEASE); return false; }
    *(int*)(tramp + kLen + 1) = (int)((p + kLen) - (tramp + kLen + 5));
    g_origMenuShow = (MenuShowFn)tramp;

    ::memcpy(g_menuShowOrigBytes, p, kLen);

    if (!FitsRel32(p, (const void*)&MenuShowDetour, L"CMenu::Show detour")) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    BYTE patch[16];
    patch[0] = 0xE9;
    *(int*)(patch + 1) = (int)((BYTE*)&MenuShowDetour - (p + 5));
    for (int i = 5; i < kLen; i++) patch[i] = 0x90;

    g_menuShowTarget = p;
    FreezeThreads();
    bool ok = WriteMemory(p, patch, kLen);
    UnfreezeThreads();
    if (!ok) {
        LogW(L"CMenu::Show patch failed");
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        g_origMenuShow = nullptr;
        return false;
    }

    LogW(L"target: CMenu::Show RVA=0x%X", (unsigned)(p - (BYTE*)g_sampBase));
    g_menuShowHooked = true;
    return true;
}

bool HookMenu()
{
    if (!g_sampBase) return false;
    BYTE* base = nullptr; DWORD size = 0;
    if (!GetTextSection(g_sampBase, &base, &size)) return false;

    BYTE* p = FindPattern(base, size, kMenuPat, kMenuMask, 32);
    if (!p) { LogW(L"signature for the menu text encoder not found"); return false; }

    g_menuTarget = p;
    g_menuBuf = (BYTE*)*(DWORD*)(p + 1);        // mov eax, <display list>
    g_menuObj = (DWORD*)*(DWORD*)(p + 7);       // mov ecx, [<object>]
    if (!g_menuBuf || !g_menuObj) { LogW(L"menu encoder operands look wrong"); return false; }

    BYTE* tramp = (BYTE*)::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    ::memcpy(tramp, p, 5);
    tramp[5] = 0xE9;
    if (!FitsRel32(tramp + 5, p + 5, L"menu encoder trampoline jump back")) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    *(int*)(tramp + 6) = (int)((p + 5) - (tramp + 10));
    g_origMenuEncoder = (MenuEncoderFn)tramp;

    ::memcpy(g_menuOrigBytes, p, 5);

    if (!FitsRel32(p, (const void*)&MenuEncoderDetour, L"menu encoder detour")) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    BYTE patch[5];
    patch[0] = 0xE9;
    *(int*)(patch + 1) = (int)((BYTE*)&MenuEncoderDetour - (p + 5));

    FreezeThreads();
    bool ok = WriteMemory(p, patch, 5);
    UnfreezeThreads();
    if (!ok) {
        LogW(L"menu encoder patch failed");
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        g_origMenuEncoder = nullptr;
        return false;
    }

    LogW(L"target: menu text encoder RVA=0x%X, display list at %p",
         (unsigned)(p - (BYTE*)g_sampBase), (void*)g_menuBuf);
    LogW(L"menu text encoder hooked (diagnostics only)");
    g_menuHooked = true;
    return true;
}



// ---------------------------------------------------------------------------
// GTA CFont::PrintString (gta_sa.exe 1.0 US, 0x71A700)
//
// Menus are not drawn by samp.dll: CMenu::Show only builds a GTA panel out of
// sprites (the display lists above are "SAMPHED" / "SAMPRW1" / "SAMP000"...),
// and the panel text is drawn by the game itself through CFont::PrintString.
// That is why a Chinese item arrives as mojibake and why rustext can "fix"
// menus by replacing the bytes before they reach the panel.
//
// The detour replaces such a line: the original is fed an equally long run of
// spaces so its pen does not move, and the text is drawn by the overlay at the
// same position and size.
// ---------------------------------------------------------------------------
typedef void(__cdecl* PrintStringFn)(float x, float y, const char* text);
static PrintStringFn g_origPrintString = nullptr;
static BYTE*          g_psTarget = nullptr;
static BYTE           g_psOrigBytes[8] = { 0 };
static bool           g_psHooked = false;
static int            g_psLogs = 0;
static BYTE*          g_gtaBase = nullptr;


// ---- ~k~~KEY~ expansion ----------------------------------------------------
//
// samp.dll does not implement it: CTextDraw::Draw checks a per-object flag and calls
// a thunk (samp.dll+0x9FD90) that is "mov eax, 69E160h; jmp eax" - it forwards to
// gta_sa.exe, which turns the control name into the label of the CURRENT binding
// ("LMB", "F", ...). Calling the same function is the only way to stay correct when a
// player rebinds a key.
typedef void (__cdecl *ExpandKeyFn)(char* text);
static ExpandKeyFn g_expandKey = nullptr;

// The expander lives in gta_sa.exe, not in samp.dll: the client's TextDraw
// renderer forwards to it through a two-instruction stub ("mov eax, <fn>; jmp
// eax") whose offset differs in every client build. Finding the expander itself
// by its prologue works for every SA-MP version and needs no per-build table:
//
//   8B 54 24 04        mov  edx, [esp+4]        ; char* text
//   81 EC 30 03 00 00  sub  esp, 0x330
//   55                 push ebp
//   33 ED              xor  ebp, ebp
//
// The signature is unique in the executable, and a miss only means the key name
// is shown literally, so nothing is called unless it matched.
static const BYTE kKeyXpPat[13] = {
    0x8B, 0x54, 0x24, 0x04, 0x81, 0xEC, 0x30, 0x03, 0x00, 0x00, 0x55, 0x33, 0xED,
};
static const char kKeyXpMask[13] = { 'x','x','x','x','x','x','x','x','x','x','x','x','x' };

static void ResolveKeyExpander()
{
    if (g_expandKey) return;
    // g_gtaBase is set by the menu probe, which is optional: with [menu]
    // enabled = off the expander still has to work, so look the module up here.
    if (!g_gtaBase) g_gtaBase = (BYTE*)::GetModuleHandleW(L"gta_sa.exe");
    if (!g_gtaBase) return;
    BYTE* base = nullptr; DWORD size = 0;
    if (!GetTextSection((HMODULE)g_gtaBase, &base, &size)) return;

    BYTE* p = FindPattern(base, size, kKeyXpPat, kKeyXpMask, 13);
    if (!p) {
        LogW(L"~k~ expander not found, key names are shown literally");
        return;
    }
    g_expandKey = (ExpandKeyFn)p;
    LogW(L"~k~ expander: gta_sa rva 0x%X", (unsigned)(p - g_gtaBase));
}

// Expands ~k~~NAME~ in place; returns `text` unchanged when the expander is not there.
static const char* ExpandKeys(const char* text, char* buf, size_t size)
{
    if (!text || !buf || size == 0) return text;
    ::lstrcpynA(buf, text, (int)size);
    if (!::strstr(buf, "~k~")) return buf;

    ResolveKeyExpander();
    if (!g_expandKey) return buf;
    __try {
        g_expandKey(buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return buf;
}


// sub esp,0x10 / push ebx / mov ebx,[esp+0x20] / mov al,[ebx] / cmp al,'*'
static const BYTE kPsPat[12] = {
    0x83, 0xEC, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x20, 0x8A, 0x03, 0x3C, 0x2A,
};
static const char kPsMask[12] = {
    'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x',
};

static bool HasHighByte(const char* s)
{
    for (int i = 0; s[i] && i < 512; i++) if ((unsigned char)s[i] >= 0x80) return true;
    return false;
}

// Gate for the takeover: the string must live inside the CMenu object that
// CMenu::Show was last called on. Every menu string (title, column headers,
// items) is a char array inside that object, so a range check is exact and,
// unlike a caller address, it does not depend on the gta_sa.exe build.
static bool InMenuObject(const char* text)
{
    if (!g_menuSelf || !text) return false;
    const BYTE* b = (const BYTE*)g_menuSelf;
    const BYTE* p = (const BYTE*)text;
    return p > b && p < b + 0x400;
}

// Not every menu string lives inside CMenu: the rows are drawn from copies the
// game keeps in its panel, which are outside that object. A short window that
// CMenu::Show opens (and every taken-over line refreshes) covers those copies
// without ever touching text drawn when no menu is open.
// True while the game is drawing a native menu. render.cpp uses it to keep the
// GameText timers running: SA-MP raises CTimer::m_CodePause for its menus, which
// would otherwise freeze a message until the menu closes.
bool MenuWindowActive()
{
    return (DWORD)(g_menuUntil - ::GetTickCount()) < 3000u;   // time left, wrap-safe
}
static bool InMenuWindow(const char* text)
{
    // Inside CMenu's own buffer: the title, and any string the game keeps there.
    if (InMenuObject(text)) return true;
    // A row is drawn from a copy the game keeps in its panel, outside that buffer, so the
    // object test cannot see it. Rows are only ever drawn while the window is open, which is
    // what MenuWindowActive() covers; the font-state signature checked by the caller is what
    // separates a row from the HUD text the game keeps drawing meanwhile.
    return MenuWindowActive();
}

// While the window is open, only text drawn with the font state the menu itself
// uses is taken over. The menu draws its title with one (style, scale) pair and
// its rows with another, so a couple of tuples cover it; the scoreboard, 3D
// labels and anything else use different ones and stay untouched.
struct MenuFontSig { int style; float sx, sy; };
static MenuFontSig g_menuSigs[6];
static int         g_menuSigCount = 0;

static bool MenuSigKnown(int style, float sx, float sy)
{
    for (int i = 0; i < g_menuSigCount; i++) {
        const float e = 0.01f;
        if (g_menuSigs[i].style == style &&
            ::fabs(g_menuSigs[i].sx - sx) < e && ::fabs(g_menuSigs[i].sy - sy) < e) return true;
    }
    if (g_menuSigCount == 0) {   // first line of this menu: it defines the set
        g_menuSigs[0].style = style; g_menuSigs[0].sx = sx; g_menuSigs[0].sy = sy;
        g_menuSigCount = 1;
        return true;
    }
    if (g_menuSigCount < 6) {
        g_menuSigs[g_menuSigCount].style = style;
        g_menuSigs[g_menuSigCount].sx = sx;
        g_menuSigs[g_menuSigCount].sy = sy;
        g_menuSigCount++;
        return true;   // the line that teaches us a new pair is drawn now
    }
    return false;
}

static void __cdecl PrintStringDetour(float x, float y, const char* text)
{
    if (!g_origPrintString) return;        // hook installed without a trampoline

    // Never while our own TextDraw path is inside the game's draw: that string is
    // already handled there.
    // Only text the client's own font cannot draw was taken at first, which left every pure-ASCII
    // menu row to the client: in a two-column menu the row "12345 ABC" came out in the game's font
    // beside rows this mod had drawn. [encoding] redraw_ascii decides that now, the same way it
    // does for TextDraw and GameText. The window and font-state gates below still apply, so text
    // drawn while a menu happens to be open is not caught by this.
    const bool want = HasHighByte(text) || g_cfg.redrawAscii;
    // A pen outside the panel CMenu::Show laid out is not a menu line. The HUD keeps drawing
    // through this same font while a menu is open, and those strings have to stay put.
    const bool inPanel = g_menuPanel.right > g_menuPanel.left &&
                         x >= (float)g_menuPanel.left && x <= (float)g_menuPanel.right &&
                         y >= (float)g_menuPanel.top && y <= (float)g_menuPanel.bottom;
    const bool inWindow = text && g_cfg.menuEnabled && g_textDrawDepth == 0 &&
                          want && (InMenuObject(text) || (InMenuWindow(text) && inPanel));
    if (!inWindow) {
        if (!MenuWindowActive()) g_menuSigCount = 0;
        g_origPrintString(x, y, text);
        return;
    }

    const BYTE* gta = g_gtaBase;
    float scaleX = *(float*)(gta + (0xC71A64 - 0x400000));
    float scaleY = *(float*)(gta + (0xC71A68 - 0x400000));
    BYTE  style  = *(BYTE*)(gta + (0xC71A94 - 0x400000));
    BYTE  r = *(BYTE*)(gta + (0xC71A60 - 0x400000));
    BYTE  g = *(BYTE*)(gta + (0xC71A61 - 0x400000));
    BYTE  b = *(BYTE*)(gta + (0xC71A62 - 0x400000));

    // Text with the menu's own font state only: the scoreboard, 3D labels and
    // the rest are drawn with different (style, scale) pairs.
    if (!MenuSigKnown((int)style, scaleX, scaleY)) {
        g_origPrintString(x, y, text);
        return;
    }
    g_menuUntil = (LONG)::GetTickCount() + 800;   // the menu is still drawing

    // A same-length run of spaces: the game keeps advancing its pen, centring
    // and drawing the selection highlight exactly as before, but the unreadable
    // glyphs are gone and our own text goes in their place.
    size_t n = ::strlen(text);
    std::string blank(n, ' ');
    g_origPrintString(x, y, blank.c_str());

    std::wstring w = ToWide(text);
    if (w.empty()) return;

    // GTA's fonts differ in base height, and the height a call produces is
    // scaleY times that base. The title (style 0) uses the tall face, not the
    // small one: measured on the same menu, the client's title ink is 40 px where
    // the small face gave 20 px, and a first pass at 2x the small base overshot to
    // 45 px, so the base is 40/45 of that.
    static const float kBase[3] = { 20.5f, 20.0f, 20.0f };
    const bool isTitle = (style == 0);
    int st = (style < 3) ? (int)style : 1;
    int px = (int)(scaleY * kBase[st] * g_cfg.menuGlyph + 0.5f);
    if (px < 6) px = 6;
    if (px > 200) px = 200;

    // The client's title face and ours do not share proportions: at the same
    // height its glyphs are narrower per letter but spaced wider, so no single
    // factor reproduces both. The height is matched by kBase above; the width is
    // [menu] title_width, calibrated so the title occupies the horizontal space
    // the client gives it (71 px for "hello" at 40 px tall, measured on the same
    // menu). Lower it for narrower letters, raise it for wider ones.
    const float aspect  = isTitle ? g_cfg.menuTitleWidth : 1.0f;
    const float advance = aspect;

    float drawY = y;
    if (isTitle) {
        px = (int)(px * g_cfg.menuTitleScale + 0.5f);
        if (px < 6) px = 6;
        if (px > 240) px = 240;
        drawY = y + g_cfg.menuTitleDy;
    }
    if (g_cfg.verbose) {
        LogDbg(L"menu string: style=%d scale=%.2f/%.2f pen=(%.1f,%.1f) px=%d",
               (int)style, scaleX, scaleY, x, y, px);
    }

    D3DCOLOR base = g_cfg.menuColor ? (0xFF000000u | g_cfg.menuColor)
                                   : (0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | b);
    std::vector<TextRun> runs = ParseColorRuns(w, base, g_cfg.braceColors);

    struct Seg { std::wstring text; D3DCOLOR color; int font; int width; int symbol; };
    std::vector<Seg> segs;
    const int menuFace = (style == 0) ? g_cfg.menuTitleFont : g_cfg.menuFont;
    for (size_t i = 0; i < runs.size(); i++) {
        std::vector<DrawPiece> dp;
        SplitRunIntoPieces(runs[i].text, px, menuFace, dp);
        for (size_t k = 0; k < dp.size(); k++) {
            segs.push_back(Seg{ dp[k].text, runs[i].color, dp[k].font, dp[k].width, dp[k].symbol });
        }
    }

    // Every advance has to be squeezed by the same factor, otherwise the next run
    // of a mixed Latin/CJK line starts where the unsqueezed text would have ended
    // and a gap opens up between the runs.
    const float hs  = g_cfg.menuHScale;

    // A symbol keeps its own square: the menu squeeze applies to glyphs only.
    auto advanceOf = [hs, advance](int width, int symbol) {
        return (float)width * (symbol ? 1.0f : hs * advance);
    };
    float totF = 0.0f;
    for (size_t i = 0; i < segs.size(); i++) totF += advanceOf(segs[i].width, segs[i].symbol);
    const int tot = (int)(totF + 0.5f);

    float penX = x;
    if (g_cfg.menuAlign == 1) penX = x - (float)tot * 0.5f;   // centre
    else if (g_cfg.menuAlign == 2) penX = x - (float)tot;      // right

    // The outline is drawn as that many pixels around the glyphs, so a fixed number
    // looks thin on the title (drawn much larger) and right on the rows. Both are
    // therefore read at a 20-px reference size and scaled, and the title has its own
    // pair: it is a heading, not a row.
    const int refOutline = isTitle ? g_cfg.menuTitleOutline : g_cfg.menuOutline;
    const int refShadow  = isTitle ? g_cfg.menuTitleShadow  : g_cfg.menuShadow;
    int outlinePx = (int)((float)refOutline * (float)px / 20.0f + 0.5f);
    if (refOutline > 0 && outlinePx < 1) outlinePx = 1;
    // Same reference size for the shadow, so it keeps its distance from the outline
    // at every menu font size instead of disappearing into it on the rows.
    int shadowPx = (int)((float)refShadow * (float)px / 20.0f + 0.5f);
    if (refShadow > 0 && shadowPx < 1) shadowPx = 1;

    // Remember this draw so the TextDraw copy of the same title is skipped.
    NoteMenuDraw(penX, drawY, px);
    NoteMenuFrame();   // this frame is a menu frame: the overlay may erase

    for (size_t i = 0; i < segs.size(); i++) {
        DrawItem it;
        it.text   = segs[i].text;
        it.x      = penX;
        it.y      = drawY;
        it.px     = px;
        it.fontId = segs[i].font;
        it.color  = segs[i].color;
        it.color2 = 0xFF000000u;                       // outline stays opaque
        it.shadowColor = ((DWORD)g_cfg.menuShadowAlpha << 24) | (g_cfg.menuShadowColor & 0x00FFFFFFu);
        it.outlineSize = outlinePx;           // the game outlines its menu text
        it.scaleX      = hs * aspect;         // GDI metrics are a touch wider than the client's
        it.shadowSize  = shadowPx;
        it.menu        = true;            // drawn below the scoreboard panel
        if (segs[i].symbol) {
            it.sprite      = segs[i].symbol;
            it.text        = ArrowFallback(segs[i].symbol);
            it.px          = segs[i].width;
            it.y           = drawY + (float)SymbolOffsetPx(px);
            it.outlineSize = 0;
            it.shadowSize  = 0;
        }
        PushDrawItem(it);
        penX += advanceOf(segs[i].width, segs[i].symbol);
    }

    if (g_psLogs < 24) {
        g_psLogs++;
        wchar_t raw[64], dec[64];
        raw[0] = 0; dec[0] = 0;
        for (size_t k = 0; k < n && k < 12; k++) {
            wchar_t one[8];
            swprintf_s(one, L"%02X", (unsigned char)text[k]);
            wcscat_s(raw, one);
        }
        for (size_t k = 0; k < w.size() && k < 12; k++) {
            wchar_t one[8];
            swprintf_s(one, L"%04X", (unsigned)w[k]);
            wcscat_s(dec, one);
        }
        LogDbg(L"menu text: x=%.1f y=%.1f px=%d style=%d scale=(%.2f,%.2f) rgb=%02X%02X%02X "
             L"raw=%s dec=%s",
             x, y, px, (int)style, scaleX, scaleY, r, g, b, raw, dec);
    }
}

bool HookPrintString()
{
    g_gtaBase = (BYTE*)::GetModuleHandleW(L"gta_sa.exe");
    if (!g_gtaBase) { LogW(L"gta_sa.exe not found, no menu text probe"); return false; }

    BYTE* base = nullptr; DWORD size = 0;
    if (!GetTextSection((HMODULE)g_gtaBase, &base, &size)) return false;

    BYTE* p = FindPattern(base, size, kPsPat, kPsMask, 12);
    if (!p) { LogW(L"signature for CFont::PrintString not found"); return false; }

    const int kLen = 8;                      // sub esp,0x10 / push ebx / mov ebx,[esp+0x20]
    BYTE* tramp = (BYTE*)::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    ::memcpy(tramp, p, kLen);
    tramp[kLen] = 0xE9;
    if (!FitsRel32(tramp + kLen, p + kLen, L"trampoline jump back")) { ::VirtualFree(tramp, 0, MEM_RELEASE); return false; }
    *(int*)(tramp + kLen + 1) = (int)((p + kLen) - (tramp + kLen + 5));
    g_origPrintString = (PrintStringFn)tramp;

    ::memcpy(g_psOrigBytes, p, kLen);

    if (!FitsRel32(p, (const void*)&PrintStringDetour, L"CFont::PrintString detour")) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        g_origPrintString = nullptr;
        return false;
    }

    BYTE patch[16];
    patch[0] = 0xE9;
    *(int*)(patch + 1) = (int)((BYTE*)&PrintStringDetour - (p + 5));
    for (int i = 5; i < kLen; i++) patch[i] = 0x90;

    g_psTarget = p;
    FreezeThreads();
    bool ok = WriteMemory(p, patch, kLen);
    UnfreezeThreads();
    if (!ok) {
        LogW(L"CFont::PrintString patch failed");
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        g_origPrintString = nullptr;
        return false;
    }

    LogW(L"target: CFont::PrintString RVA=0x%X (gta_sa.exe)", (unsigned)(p - g_gtaBase));
    LogW(L"menu text probe installed (logs non-ASCII text drawn by the game)");
    g_psHooked = true;
    return true;
}

// ---------------------------------------------------------------------------
// Client UI text: CFonts::DrawText / CFonts::DrawLittleText (0.3.DL)
//
// The reconstructed 0.3.DL source (D:\Work\reverse-samp) names these two:
//
//   class CFonts {                     // g_deviceFonts
//       ID3DXFont *m_pFont, *m_pLittleFont, *m_pShadow, *m_pLittleShadow;
//       ID3DXFont* m_pLicensePlateFont; ID3DXSprite* m_pDefaultSprite;
//       IDirect3DDevice9* m_pDevice; char* m_szTempBuffer;
//       int m_nFontHeight, m_nLittleFontHeight;      // offsets 32 and 36
//       void DrawText(ID3DXSprite*, const char*, RECT, D3DCOLOR, BOOL);              // 0x6ada0
//       void DrawLittleText(ID3DXSprite*, const char*, RECT, int, D3DCOLOR, BOOL);   // 0x6af20
//   };
//
// Everything the client's own UI paints (scoreboard, dialogs and the F1 help box,
// 3D labels, nametags, chat bubbles, spawn screen, download window, net stats) goes
// through one of these two, with the RECT, the DT_* format and the colour supplied by
// the caller - which is exactly what a takeover needs, and why the earlier attempt at
// guessing sizes from the box was wrong: the real font height sits in the object.
//
// This is the probe stage: it only logs, and forwards everything to the trampoline.
static const BYTE kClientTextPat[11] = {
    0x51, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x85, 0xFF, 0x8B, 0xF1,
};
static const char kClientTextMask[11] = {
    'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x',
};
static const int  kClientTextLen = 11;

typedef void(__fastcall* CFontsDrawTextFn)(void* self, void* unused, void* sprite,
                                           const char* text, long l, long t, long r, long b,
                                           DWORD color, int shadow);
typedef void(__fastcall* CFontsDrawLittleFn)(void* self, void* unused, void* sprite,
                                             const char* text, long l, long t, long r, long b,
                                             int format, DWORD color, int shadow);

static void __fastcall ClientTextDetour0(void* self, void* unused, void* sprite,
                                         const char* text, long l, long t, long r, long b,
                                         DWORD color, int shadow);
static void __fastcall ClientTextDetour1(void* self, void* unused, void* sprite,
                                         const char* text, long l, long t, long r, long b,
                                         int format, DWORD color, int shadow);

static const int kClientProlog = 7;
static BYTE* g_ctTarget[2] = { nullptr, nullptr };
static BYTE* g_ctTramp[2]  = { nullptr, nullptr };
static BYTE  g_ctOrig[2][8] = {};
static int   g_ctCount = 0;
static DWORD g_ctLogged = 0;

static void LogClientText(int which, void* self, const char* text, long l, long t,
                          long r, long b, DWORD colour, long format, DWORD caller)
{
    if (!g_cfg.verbose) return;

    static struct { int which; char str[40]; } seen[64];
    static int seenCount = 0;

    char head[40] = { 0 };
    char shown[40] = { 0 };
    wchar_t hex[50] = { 0 };
    bool ok = false;
    __try {
        if (text) {
            for (int i = 0; i < 39 && text[i]; i++) {
                unsigned char c = (unsigned char)text[i];
                shown[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
                head[i]  = shown[i];
            }
            for (int i = 0; i < 16 && text[i]; i++) {
                wchar_t one[8];
                swprintf_s(one, L"%02X ", (unsigned char)text[i]);
                wcscat_s(hex, one);
            }
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) return;

    for (int i = 0; i < seenCount; i++) {
        if (seen[i].which == which && ::strncmp(seen[i].str, head, 39) == 0) return;
    }
    if (seenCount >= (int)(sizeof(seen) / sizeof(seen[0]))) return;
    seen[seenCount].which = which;
    ::strncpy_s(seen[seenCount].str, head, 39);
    seenCount++;
    g_ctLogged++;

    int fontH = 0, littleH = 0;
    if (self) {
        fontH   = *(int*)((BYTE*)self + 32);
        littleH = *(int*)((BYTE*)self + 36);
    }

    LogDbg(L"ui text: %s ret=+0x%X rect=(%ld,%ld)-(%ld,%ld) fmt=%03lX colour=%08X font=%d/%d hex=[%s] str=\"%S\"",
           which ? L"little" : L"text",
           (unsigned)(caller - (DWORD)g_sampBase),
           l, t, r, b, format, colour, fontH, littleH, hex, shown);
}

static void __fastcall ClientTextDetour(void* self, void* unused, void* sprite,
                                        const char* text, long l, long t, long r, long b,
                                        DWORD color, int shadow);

bool HookClientText()
{
    if (!g_sampBase) return false;
    BYTE* base = nullptr; DWORD size = 0;
    if (!GetTextSection((HMODULE)g_sampBase, &base, &size)) return false;

    // The two call sites differ only in their branch displacements, so the shared
    // 11-byte prologue finds both; they are told apart by their position (the lower
    // address is DrawText, the higher DrawLittleText in every build checked).
    BYTE* found[2] = { nullptr, nullptr };
    BYTE* p = base;
    int n = 0;
    while (n < 2) {
        p = FindPattern(p, (DWORD)((base + size) - p), kClientTextPat, kClientTextMask, kClientTextLen);
        if (!p) break;
        found[n++] = p;
        p += kClientTextLen;
    }
    if (n < 2) { LogW(L"client text signatures not found (%d)", n); return false; }

    LogW(L"client text: DrawText at samp.dll+0x%X, DrawLittleText at samp.dll+0x%X",
         (unsigned)(found[0] - (BYTE*)g_sampBase), (unsigned)(found[1] - (BYTE*)g_sampBase));

    void* detours[2] = { (void*)&ClientTextDetour0, (void*)&ClientTextDetour1 };
    for (int i = 0; i < 2; i++) {
        BYTE* target = found[i];
        BYTE* tramp = (BYTE*)::VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
        if (!tramp) return false;
        ::memcpy(tramp, target, kClientProlog);
        tramp[kClientProlog] = 0xE9;
        if (!FitsRel32(tramp + kClientProlog, target + kClientProlog, L"client text trampoline")) {
            ::VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }
        *(int*)(tramp + kClientProlog + 1) = (int)((target + kClientProlog) - (tramp + kClientProlog + 5));

        ::memcpy(g_ctOrig[i], target, kClientProlog);
        g_ctTramp[i] = tramp;
        g_ctTarget[i] = target;

        if (!FitsRel32(target, detours[i], L"client text detour")) return false;
        BYTE patch[8];
        patch[0] = 0xE9;
        *(int*)(patch + 1) = (int)((BYTE*)detours[i] - (target + 5));
        patch[5] = 0x90; patch[6] = 0x90;

        FreezeThreads();
        bool ok = WriteMemory(target, patch, kClientProlog);
        UnfreezeThreads();
        if (!ok) { LogW(L"client text patch failed"); return false; }
    }
    g_ctCount = 2;
    LogW(L"client text hooks installed (log only)");
    return true;
}

void UnhookClientText()
{
    for (int i = 0; i < g_ctCount; i++) {
        if (g_ctTarget[i]) {
            FreezeThreads();
            WriteMemory(g_ctTarget[i], g_ctOrig[i], kClientProlog);
            UnfreezeThreads();
        }
        if (g_ctTramp[i]) ::VirtualFree(g_ctTramp[i], 0, MEM_RELEASE);
        g_ctTramp[i] = nullptr;
        g_ctTarget[i] = nullptr;
    }
    g_ctCount = 0;
}

// Does this line need this mod's font? The client's own glyphs are by definition the
// original look, so a line it can draw is left alone; only bytes above ASCII (or the
// caller asking for everything) are taken over.
bool ClientTextWanted(const char* text, int len)
{
    if (!g_cfg.clientText) return false;
    if (!text || len <= 0) return false;
    for (int i = 0; i < len; i++) {
        if ((unsigned char)text[i] >= 0x80) return true;   // not drawable by the client
    }
    if (g_cfg.clientTextAsciiOurs) return true;
    // A short plain label is a control (a button caption, a column header): the client
    // renders those with its own translucent shadow and state colours, which is exactly
    // what should stay. A longer plain line is a chat or message line and is drawn here,
    // so that one chat box does not mix two renderers line by line.
    return len > 8;
}

// The string comes from the client, so reading it is guarded; kept out of the caller
// because __try cannot share a frame with C++ objects.
static int SafeStrLen(const char* text, int maxLen)
{
    __try {
        return text ? (int)::strnlen(text, (size_t)maxLen) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}


// The project does not link the D3DX SDK headers, so the little of ID3DXFont this
// needs is declared here: the interface is opaque, only its vtable slots are used.
struct ID3DXFont;
struct ID3DXSprite;
typedef struct D3DXFONT_DESCA
{
    INT  Height;
    UINT Width;
    UINT Weight;
    UINT MipLevels;
    BOOL Italic;
    BYTE CharSet;
    BYTE OutputPrecision;
    BYTE Quality;
    BYTE PitchAndFamily;
    CHAR FaceName[LF_FACESIZE];
} D3DXFONT_DESCA;
typedef HRESULT(STDMETHODCALLTYPE* FontGetDescAFn)(ID3DXFont*, D3DXFONT_DESCA*);

// The face behind a font pointer, so a log line can say which one drew it: the death
// window paints a name through a face of its own and the question is which of the
// passes belongs to it. Cached because this sits in the render path.
static const D3DXFONT_DESCA* LookupFontDesc(ID3DXFont* font)
{
    struct Slot { ID3DXFont* font; D3DXFONT_DESCA desc; };
    static Slot cache[6] = {};
    static int  next = 0;
    if (!font) return nullptr;
    for (int i = 0; i < 6; i++)
        if (cache[i].font == font) return &cache[i].desc;
    FontGetDescAFn getDesc = (FontGetDescAFn)(*(void***)font)[4];
    if (!getDesc) return nullptr;
    D3DXFONT_DESCA desc = {};
    if (FAILED(getDesc(font, &desc))) return nullptr;
    Slot& slot = cache[next];
    next = (next + 1) % 6;
    slot.font = font;
    slot.desc = desc;
    return &slot.desc;
}

// Where a return address lives. Subtracting the samp base from every caller printed a
// nine digit number for the passes that come from elsewhere - and those are exactly the
// lines the death window question is read from.
static void DescribeCaller(const BYTE* addr, wchar_t* out, size_t cch)
{
    const BYTE* samp = (const BYTE*)g_sampBase;
    const BYTE* game = (const BYTE*)::GetModuleHandleA(nullptr);
    if (samp && addr >= samp && addr < samp + 0x200000)
        _snwprintf_s(out, cch, _TRUNCATE, L"+0x%X", (unsigned)(addr - samp));
    else if (game && addr >= game && addr < game + 0x800000)
        _snwprintf_s(out, cch, _TRUNCATE, L"gta+0x%X", (unsigned)(addr - game));
    else
        _snwprintf_s(out, cch, _TRUNCATE, L"0x%08X", (unsigned)(DWORD_PTR)addr);
    out[cch - 1] = 0;
}

// The samp.dll RVA of a return address, or a value no range matches when the caller
// does not live in samp.dll at all.
static DWORD CallerRva(const BYTE* addr)
{
    const BYTE* samp = (const BYTE*)g_sampBase;
    if (samp && addr >= samp && addr < samp + 0x200000) return (DWORD)(addr - samp);
    return 0xFFFFFFFFu;
}

// The shadow pass of a chat line is drawn with its {RRGGBB} tags already removed
// (ChatStripColorTags runs first), so the body has to be reduced the same way before the
// two can be compared. Comparing the tagged line matched only the untagged ones - the
// timestamp got its outline and every coloured message looked shadowless, which is why
// F7 appeared to do nothing.
static void StripChatColourTags(const char* src, int len, char* dst, size_t cap)
{
    size_t o = 0;
    for (int i = 0; i < len && src[i]; ) {
        if (src[i] == '{' && i + 7 < len && src[i + 7] == '}') {
            bool hex = true;
            for (int k = 1; k <= 6; k++) {
                const char c = src[i + k];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                    hex = false;
                    break;
                }
            }
            if (hex) { i += 8; continue; }
        }
        if (o + 1 >= cap) break;
        dst[o++] = src[i++];
    }
    dst[o] = 0;
}

// ---------------------------------------------------------------------------
// The fonts are found through d3dx9, not through a samp.dll address: the launcher
// rotates client builds (this session got a 0.3.8.0 after 0.3.DL), so any hard-coded
// RVA is wrong on the next launch - and patching the wrong vtable crashes the game.
// Every ID3DXFont is created by one of the four D3DXCreateFont* exports and they all
// share the vtable d3dx9 builds, so wrapping the first created font covers the
// client's fonts, DXUT's (list box, static text, buttons), the chat's and the
// license-plate / object-material ones alike.
// ---------------------------------------------------------------------------
typedef INT(STDMETHODCALLTYPE* FontDrawTextAFn)(ID3DXFont*, ID3DXSprite*, const char*,
                                                INT, RECT*, DWORD, D3DCOLOR);
typedef INT(STDMETHODCALLTYPE* FontDrawTextWFn)(ID3DXFont*, ID3DXSprite*, const wchar_t*,
                                                INT, RECT*, DWORD, D3DCOLOR);

static FontDrawTextAFn g_fontOrigA = nullptr;
static FontDrawTextWFn g_fontOrigW = nullptr;
static bool g_fontVtHooked = false;

// Counted once a second so the log answers "did the font hook fire at all?" without
// switching the whole takeover to log mode.
static LONG g_fontTaken   = 0;
static LONG g_fontDropped = 0;
static LONG g_fontPassed  = 0;
static char g_lastShadowText[96] = { 0 };
static DWORD g_lastShadowTick = 0;
// Which font call set the record. A shadow is only meaningful for the body drawn right
// after it: with F7 in the no-shadow state nothing is drawn at all, so without this the
// record left over from the previous frame made the first line of the re-render claim a
// shadow it does not have.
static volatile LONG g_fontCallSeq  = 0;
static volatile LONG g_lastShadowSeq = 0;

// ---------------------------------------------------------------------------
// Which call sites paint a shadow
//
// A shadow pass is a black copy of a line. Its site is learned rather than looked up: the
// pass is remembered with its caller, and when the body for that same text arrives - the
// chat through the wrapper, the death window through its own counter - those callers are
// known to be shadow sites for the rest of the session. A build that shifts its code
// therefore needs no new table: it learns its own sites from the first line it draws, at
// the cost of that one line still showing the client's copies.
// ---------------------------------------------------------------------------
struct ShadowPass { const void* font; DWORD rva; DWORD tick; char text[96]; };
static ShadowPass g_shadowPasses[16] = {};
static int   g_shadowPassNext = 0;
static DWORD g_shadowSites[12] = { 0 };
static int   g_shadowSiteCount = 0;

// The wrapper call sites that draw the chat's body, learned the same way: the call that
// matches a shadow record is the chat's, so its caller is remembered and the next frame
// recognises it without waiting for the pairing.
static DWORD g_chatBodySites[8] = { 0 };
static int   g_chatBodySiteCount = 0;

// ---------------------------------------------------------------------------
// Call sites, per client build
//
// The client paints a chat line by calling its font eight times at eight sites - the black
// copies - and once at a ninth, the body. Dropping the copies means recognising those sites,
// and every build has its own addresses, so they are keyed on the client's link time and file
// size rather than a version number.
//
// The seven clients the launcher ships are measured and listed here, which is what makes the
// first chat line after joining clean. Anything else learns its own sites from that line: the
// copies stay beside this mod's glyphs until the line is repainted, and from then on the build
// behaves like a measured one. Nothing is cached on disk, because the game folder sits under
// Program Files and the write would be refused.
struct KnownBuildSites { DWORD stamp; DWORD size; const char* shadow; const char* chat; };
static const KnownBuildSites kKnownBuildSites[] = {
    { 0x5542F47A, 2199552, "63967,639A4,639D5,63A18,63A55,63A86,63AC9,63B02", "63BEA" },  // 0.3.7-R1
    { 0x59C30C94, 2220032, "63A37,63A74,63AA5,63AE8,63B25,63B56,63B99,63BD2", "63CBA" },  // 0.3.7-R2
    { 0x5C08E5BC, 1204224, "66DB7,66DF4,66E25,66E68,66EA5,66ED6,66F19,66F52", "6703A" },  // 0.3.7-R3
    { 0x5C0B4243, 1204224, "66DB7,66DF4,66E25,66E68,66EA5,66ED6,66F19,66F52", "6703A" },  // 0.3.7-R3-1
    { 0x5DD606CD, 1204224, "674F7,67534,67565,675A8,675E5,67616,67659,67692", "6777A" },  // 0.3.7-R4
    { 0x6372C39E, 1204224, "67537,67574,675A5,675E8,67625,67656,67699,676D2", "677BA" },  // 0.3.7-R5
    { 0x5A6A3130, 1466368, "66FA7,66FE4,67015,67058,67095,670C6,67109,67142", "6722A" },  // 0.3.DL
};

// "63967,639A4,..." -> the array, skipping anything already known.
static int ParseSiteList(const char* csv, DWORD* out, int count, int cap)
{
    const char* p = csv;
    while (p && *p && count < cap) {
        char* end = nullptr;
        const unsigned v = (unsigned)::strtoul(p, &end, 16);
        if (end == p) break;
        if (v) {
            bool known = false;
            for (int i = 0; i < count; i++) if (out[i] == v) { known = true; break; }
            if (!known) out[count++] = v;
        }
        p = (*end == ',') ? end + 1 : end;
        if (*p == ' ' || *p == '\r' || *p == '\n' || !*p) break;
    }
    return count;
}

// The client build the sites belong to: PE link time plus file size, which separates every
// samp.dll under the launcher's folder.
static bool ClientBuildKey(DWORD* stamp, DWORD* size)
{
    if (!g_sampBase || !stamp || !size) return false;
    *stamp = GetImageTimestamp(g_sampBase);
    *size  = 0;
    const std::wstring path = GetModulePath(g_sampBase);
    if (!path.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
            *size = fa.nFileSizeLow;
    }
    return *stamp != 0;
}

// One line per client build, so a single pass over the launcher's folder fills the table:
//   build <pe-timestamp> <file-size> shadow <rva,rva,...> chat <rva,...>
// A line for another build is kept, and the line for this build is replaced on every save.
// Fill the site table for the client that is loaded, from the measured builds below. There is
// no on-disk cache: the seven clients the launcher ships are all in the table, and anything else
// learns its own sites on its first line (see LearnShadowSite) - a cost of one line per launch,
// which a file next to the executable cannot be relied on to remove (the game folder is under
// Program Files, so the write is refused).
void SeedKnownBuildSites()
{
    DWORD stamp = 0, size = 0;
    if (!ClientBuildKey(&stamp, &size)) return;

    for (size_t i = 0; i < _countof(kKnownBuildSites); i++) {
        const KnownBuildSites& k = kKnownBuildSites[i];
        if (k.stamp != stamp || k.size != size) continue;
        g_shadowSiteCount   = ParseSiteList(k.shadow, g_shadowSites, 0, 12);
        g_chatBodySiteCount = ParseSiteList(k.chat, g_chatBodySites, 0, 8);
        LogW(L"sites: measured build %08X/%u, %d shadow and %d chat body site(s) seeded",
             (unsigned)stamp, (unsigned)size, g_shadowSiteCount, g_chatBodySiteCount);
        return;
    }
    LogW(L"sites: build %08X/%u is not one of the measured ones, its chat sites are learned "
         L"from the first line", (unsigned)stamp, (unsigned)size);
}

static bool ChatBodySiteKnown(DWORD rva)
{
    for (int i = 0; i < g_chatBodySiteCount; i++)
        if (g_chatBodySites[i] == rva) return true;
    return false;
}


static bool ShadowSiteKnown(DWORD rva)
{
    for (int i = 0; i < g_shadowSiteCount; i++)
        if (g_shadowSites[i] == rva) return true;
    return false;
}

static void LearnShadowSite(DWORD rva)
{
    if (rva == 0xFFFFFFFFu || ShadowSiteKnown(rva) || g_shadowSiteCount >= 12) return;
    g_shadowSites[g_shadowSiteCount++] = rva;
    LogW(L"shadow site learned: +0x%X (%d known, first=%X)", (unsigned)rva, g_shadowSiteCount,
         (unsigned)g_shadowSites[0]);
}

static void RememberShadowPass(const void* font, DWORD rva, const char* text)
{
    ShadowPass& p = g_shadowPasses[g_shadowPassNext];
    g_shadowPassNext = (g_shadowPassNext + 1) % 16;
    p.font = font;
    p.rva  = rva;
    p.tick = ::GetTickCount();
    int i = 0;
    for (; i < (int)sizeof(p.text) - 1 && text && text[i]; i++) p.text[i] = text[i];
    p.text[i] = 0;
}

// Called once the body of a shadowed line is this mod's: every pass remembered for that
// text within the last moment came from a shadow site.
static void ConfirmShadowPasses(const char* text)
{
    if (!text || !*text) return;
    const DWORD now = ::GetTickCount();
    for (int i = 0; i < 16; i++) {
        const ShadowPass& p = g_shadowPasses[i];
        if (!p.font || !p.text[0] || now - p.tick > 1500) continue;
        if (::strcmp(p.text, text) != 0) continue;
        LearnShadowSite(p.rva);
    }
}

// Was this font painting black copies of this text a moment ago? If so the coloured draw
// that asks is the body of a shadowed line, which is how the death window is recognised
// on a build whose callers were never measured.
static bool ShadowPassRecent(const void* font, const char* text)
{
    if (!font || !text || !*text) return false;
    const DWORD now = ::GetTickCount();
    for (int i = 0; i < 16; i++) {
        const ShadowPass& p = g_shadowPasses[i];
        if (!p.font || p.font != font || !p.text[0] || now - p.tick > 1500) continue;
        if (::strcmp(p.text, text) == 0) return true;
    }
    return false;
}

// Two of the decisions below are keyed on *which caller* drew a line: the chat's shadow
// passes and the death window's five passes. Those callers are module offsets, so they
// only hold for the build they were measured on, and a wrong window takes over the wrong
// text - which is exactly what happened when 0.3.7-R1 ran with the 0.3.DL numbers and the
// chat came out with the client's own black copies beside this mod's glyphs.
//
// Each window was read off the log (draw one chat line, one death message, and note the
// ret= of every pass) and is checked against the module's own .text before it is used, so
// a build that shifts its code simply gets no window instead of a wrong one.
struct CallerWindow { DWORD lo, hi; };
static CallerWindow g_chatWindow  = { 0, 0 };
static CallerWindow g_deathWindow = { 0, 0 };

static bool InWindow(const CallerWindow& w, DWORD rva)
{
    return w.lo && w.hi > w.lo && rva >= w.lo && rva < w.hi;
}

static bool WindowInText(const CallerWindow& w, const BYTE* text, DWORD textSize)
{
    if (!w.lo || w.hi <= w.lo || !text || !textSize || !g_sampBase) return false;
    const DWORD textRva = (DWORD)(text - (const BYTE*)g_sampBase);
    return w.lo >= textRva && w.hi <= textRva + textSize;
}

void SelectClientCallerWindows(unsigned v0, unsigned v1, unsigned v2)
{
    DWORD chatLo = 0, chatHi = 0, deathLo = 0, deathHi = 0;
    const wchar_t* build = L"unknown";
    if (v0 == 0 && v1 == 3 && v2 == 8) {                 // 0.3.DL reports 0.3.8.0
        chatLo = 0x66000;  chatHi = 0x68000;
        deathLo = 0x69000; deathHi = 0x6A800;
        build = L"0.3.DL";
    } else if (v0 == 0 && v1 == 3 && v2 == 7) {          // 0.3.7-R1, chat measured at 0x63967
        chatLo = 0x63000;  chatHi = 0x65000;
        build = L"0.3.7";
    }

    BYTE* text = nullptr;
    DWORD textSize = 0;
    GetSectionByName(g_sampBase, ".text", &text, &textSize);

    const CallerWindow chat  = { chatLo, chatHi };
    const CallerWindow death = { deathLo, deathHi };
    g_chatWindow  = WindowInText(chat,  text, textSize) ? chat  : CallerWindow{ 0, 0 };
    g_deathWindow = WindowInText(death, text, textSize) ? death : CallerWindow{ 0, 0 };
    LogW(L"caller windows: build=%s .text=%u chat=%s(%X..%X) death=%s(%X..%X)",
         build, (unsigned)textSize, g_chatWindow.lo ? L"on" : L"off", (unsigned)chat.lo,
         (unsigned)chat.hi, g_deathWindow.lo ? L"on" : L"off", (unsigned)death.lo,
         (unsigned)death.hi);
}


// The last line the wrapper drew, to recognise the second pass the client's control
// renderer makes: it calls this twice with the same rectangle and colour, once for its
// shadow and once for the text.
static std::wstring g_lastWrapperText;
static RECT         g_lastWrapperRect = { 0, 0, 0, 0 };
static DWORD        g_lastWrapperColour = 0;
static DWORD        g_lastWrapperTick = 0;
// Which caller just had a translucent shadow pass dropped, so the line that follows it can
// carry one copy of that shadow back. An edit box draws its text once and has no such pass,
// which is why it must not get a copy: a black copy under white glyphs darkened them.
static DWORD        g_controlShadowRva = 0xFFFFFFFFu;
static DWORD        g_controlShadowTick = 0;


static INT STDMETHODCALLTYPE FontDrawTextADetour(ID3DXFont*, ID3DXSprite*, const char*,
                                                 INT, RECT*, DWORD, D3DCOLOR);
static INT STDMETHODCALLTYPE FontDrawTextWDetour(ID3DXFont*, ID3DXSprite*, const wchar_t*,
                                                 INT, RECT*, DWORD, D3DCOLOR);

// One line per font call: which caller, which face, which colour, and what this mod
// did with it. The two ranges that matter are logged in full - a 200 ms throttle hid
// every pass but the first of the death window's five, which is why the duplication
// could not be read off the log - and everything else keeps the old throttle.
static void LogFontPass(const wchar_t* what, const BYTE* retAddr, ID3DXFont* font,
                        const char* text, int glyphs, RECT* rect, DWORD format, DWORD colour)
{
    if (!g_cfg.verbose || !text || glyphs <= 1) return;
    const DWORD rva = CallerRva(retAddr);
    const bool interesting = InWindow(g_chatWindow, rva) || InWindow(g_deathWindow, rva);
    static DWORD s_lastQuiet = 0;
    if (!interesting) {
        const DWORD now = ::GetTickCount();
        if (now - s_lastQuiet < 200) return;
        s_lastQuiet = now;
    }
    wchar_t caller[24];
    DescribeCaller(retAddr, caller, _countof(caller));
    const D3DXFONT_DESCA* desc = LookupFontDesc(font);
    const long boxW = rect ? (rect->right - rect->left) : 0;
    if (desc)
        LogDbg(L"font draw: ret=%s %s face=%S/%d/%d fmt=%03lX colour=%08X at=(%ld,%ld) w=%ld \"%S\"",
               caller, what, desc->FaceName, (int)desc->Height, (int)desc->Weight,
               (unsigned long)format, (unsigned)colour, rect ? rect->left : 0L,
               rect ? rect->top : 0L, boxW, text);
    else
        LogDbg(L"font draw: ret=%s %s face=? fmt=%03lX colour=%08X at=(%ld,%ld) w=%ld \"%S\"",
               caller, what, (unsigned long)format, (unsigned)colour, rect ? rect->left : 0L,
               rect ? rect->top : 0L, boxW, text);
}

static void PatchFontVtable(ID3DXFont* font)
{
    if (g_fontVtHooked || !font) return;
    void** vt = *(void***)font;
    if (!vt) return;

    DWORD old = 0;
    if (!::VirtualProtect(&vt[14], 2 * sizeof(void*), PAGE_READWRITE, &old)) return;
    g_fontOrigA = (FontDrawTextAFn)vt[14];
    g_fontOrigW = (FontDrawTextWFn)vt[15];
    vt[14] = (void*)&FontDrawTextADetour;
    vt[15] = (void*)&FontDrawTextWDetour;
    DWORD tmp = 0;
    ::VirtualProtect(&vt[14], 2 * sizeof(void*), old, &tmp);

    g_fontVtHooked = true;
    LogW(L"font vtable hooked through D3DXCreateFont: DrawTextA/DrawTextW for every font");
}

typedef HRESULT(WINAPI* CreateFontAFn)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL,
                                       DWORD, DWORD, DWORD, DWORD, LPCSTR, ID3DXFont**);
static CreateFontAFn g_createFontA = nullptr;

static HRESULT WINAPI CreateFontADetour(IDirect3DDevice9* dev, INT height, UINT width,
                                        UINT weight, UINT mip, BOOL italic, DWORD charset,
                                        DWORD precision, DWORD quality, DWORD pitch,
                                        LPCSTR face, ID3DXFont** font)
{
    if (!g_createFontA) return E_FAIL;
    HRESULT hr = g_createFontA(dev, height, width, weight, mip, italic, charset,
                               precision, quality, pitch, face, font);
    if (SUCCEEDED(hr) && font) PatchFontVtable(*font);
    return hr;
}

// Patches one exported function with a five byte jmp, keeping a trampoline. Only the
// standard "mov edi,edi; push ebp; mov ebp,esp" prologue is accepted, so a build whose
// d3dx9 differs is skipped instead of corrupted.
static bool HookExport(const char* name, void* detour)
{
    HMODULE mod = ::GetModuleHandleA("d3dx9_43.dll");
    if (!mod) {
        for (int i = 43; i >= 24 && !mod; i--) {
            char dll[32];
            wsprintfA(dll, "d3dx9_%d.dll", i);
            mod = ::GetModuleHandleA(dll);
        }
    }
    if (!mod) return false;

    BYTE* fn = (BYTE*)::GetProcAddress(mod, name);
    if (!fn) return false;
    static const BYTE kProlog[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
    if (::memcmp(fn, kProlog, 5) != 0) {
        LogW(L"d3dx9 %S: unexpected prologue, not hooked", name);
        return false;
    }

    BYTE* tramp = (BYTE*)::VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    ::memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(int*)(tramp + 6) = (int)((fn + 5) - (tramp + 10));

    BYTE patch[9];
    patch[0] = 0xE9;
    *(int*)(patch + 1) = (int)((BYTE*)detour - (fn + 5));
    patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90; patch[8] = 0x90;

    DWORD old = 0;
    if (!::VirtualProtect(fn, 12, PAGE_EXECUTE_READWRITE, &old)) return false;
    ::memcpy(fn, patch, 9);
    DWORD tmp = 0;
    ::VirtualProtect(fn, 12, old, &tmp);

    if (::strcmp(name, "D3DXCreateFontA") == 0) g_createFontA = (CreateFontAFn)tramp;
    LogW(L"hooked d3dx9 %S", name);
    return true;
}

void LogFontTextSummary()
{
    if (!g_cfg.verbose || !g_fontVtHooked) return;
    static DWORD last = 0;
    const DWORD now = ::GetTickCount();
    if (now - last < 1000) return;
    last = now;
    const LONG taken = ::InterlockedExchange(&g_fontTaken, 0);
    const LONG dropped = ::InterlockedExchange(&g_fontDropped, 0);
    const LONG passed = ::InterlockedExchange(&g_fontPassed, 0);
    if (taken || dropped || passed)
        LogDbg(L"font text: taken=%d dropped=%d passed=%d", (int)taken, (int)dropped, (int)passed);
}


static void TakeOverClientText(int which, void* self, void* sprite, const char* text,
                               long l, long t, long r, long b, DWORD colour, long format,
                               int forcePx, const wchar_t* wideText);

// ---------------------------------------------------------------------------
// CFontWrapper: the client's own ID3DXFont subclass, and the one DXUT uses for every
// static text, button and list box entry:
//
//   CFontWrapper* wrapper = new CFontWrapper;      // CDXUTDialogResourceManager::CreateFont
//   wrapper->font = node->pFont;
//   node->pFont = wrapper;
//
// So the dialog title, the OK button and the scoreboard rows all arrive at this
// vtable's DrawTextA, and so does the chat (CChat::RenderEntry calls
// m_pFont->DrawTextA with the bytes and the {RRGGBB} tags intact). The font level
// below never sees them, which is why patching d3dx9 alone changed nothing.
//
// 0.3.DL: the vtable is a static array in .rdata at samp.dll+0x11C438. It is validated
// before it is touched: every slot has to be a pointer inside the same module, so a
// build where that RVA means something else is skipped instead of corrupted.
// ---------------------------------------------------------------------------
// The class is a COM interface, so `this` arrives on the stack, not in ECX:
//   mov eax, [esp+8]        ; sprite, i.e. arg 2 after this
//   ...
//   ret 0x1C                ; seven stack arguments
// A __fastcall detour reads ECX as `this` and shifts every other argument by one -
// which is what crashed the game. The detour is __stdcall with the real signature.
typedef INT(__stdcall* WrapperDrawTextAFn)(void* self, void* sprite, const char* text,
                                           int count, RECT* rect, DWORD format,
                                           D3DCOLOR colour);

static WrapperDrawTextAFn g_wrapperOrigA = nullptr;
static bool g_wrapperHooked = false;

static INT __stdcall WrapperTextBody(void* self, void* sprite, const char* text, int count,
                                     RECT* rect, DWORD format, D3DCOLOR colour)
{
    const bool measure = (format & 0x0400) != 0;      // DT_CALCRECT

    if (g_cfg.verbose && text && rect) {
        static DWORD s_lastLog = 0;
        const DWORD now = ::GetTickCount();
        if (now - s_lastLog >= 200) {
            s_lastLog = now;
            LogW(L"wrapper: ret=+0x%X rect=(%ld,%ld)-(%ld,%ld) fmt=%03lX colour=%08X count=%d chat=%ld \"%S\"",
                 (unsigned)g_wrapperRetRva, rect->left, rect->top, rect->right, rect->bottom,
                 (unsigned long)format, (unsigned)colour, count, (long)g_wrapperChatCall, text);
        }
    }

    // Measuring calls, sprite-less calls and single glyphs stay with the client: DXUT
    // and the chat lay themselves out from the answer, and a single glyph is a symbol -
    // the death window's weapon plate and icon are drawn exactly that way, and this font
    // has no glyph for them.
    int glyphs = 0;
    if (text) glyphs = (count < 0) ? (int)::strnlen(text, 64) : count;
    // One glyph is a symbol (the death window's plate and icon) and is left to the client,
    // unless it is a printable character - a scoreboard cell is drawn one character at a
    // time and has to come from this font too.
    const bool singlePrintable = (glyphs == 1 && text &&
                                  (unsigned char)text[0] >= 0x20 &&
                                  (unsigned char)text[0] < 0x7F);
    if (!g_wrapperOrigA || g_cfg.clientText != 2 || !g_cfg.clientTextWrapper ||
        measure || !sprite || !text || !rect ||
        (glyphs <= 1 && !singlePrintable) ||
        !ClientTextWanted(text, glyphs)) {

        if (g_wrapperOrigA)
            return g_wrapperOrigA(self, sprite, text, count, rect, format, colour);
        return 0;
    }

    // The glyphs this mod draws carry their own outline, so the client's black pass
    // (DXUT draws one offset by a pixel, the chat draws four) is dropped.
    if ((colour & 0x00FFFFFFu) == 0) return 0;

    int px = 0;
    int weight = -1;
    ID3DXFont* inner = *(ID3DXFont**)((BYTE*)self + 4);
    if (inner) {
        D3DXFONT_DESCA desc = {};
        FontGetDescAFn getDesc = (FontGetDescAFn)(*(void***)inner)[4];
        if (getDesc && SUCCEEDED(getDesc(inner, &desc)) && desc.Height >= 6 && desc.Height <= 96) {
            px = desc.Height;
            // The client's chat font is bold. Drawing it at 400 looked washed out beside the
            // original, so the weight is taken from the font the client is drawing with.
            if (desc.Weight >= 100 && desc.Weight <= 900) weight = (int)desc.Weight;
        }
    }
    if (px <= 0) {
        px = (int)(rect->bottom - rect->top);
        if (px < 6 || px > 96) px = 20;
    }

    const int n = (count < 0) ? SafeStrLen(text, 8192) : count;
    if (n <= 0) return 0;

    // Does this line have a shadow? The client paints it a moment before the body, and the
    // font level has just recorded (and, once learned, dropped) it; if nothing was recorded,
    // the line is drawn without one - the chat's F7 no-shadow state - and the glyphs must
    // not carry a shadow either. Only that pairing gives a line a shadow: a button caption,
    // a table cell and the scoreboard header are drawn without one, and copies around them
    // are what made them look dark and heavy.
    int outline = 0;
    const wchar_t* why = L"no record";
    // A body that matches the shadow pass just recorded *is* the chat's, whatever caller
    // address it came from: that pairing is what the chat does and the dialogs and controls
    // do not. Keying this only on the caller window is what left an unmeasured build with
    // the client's black copies beside this mod's glyphs.
    bool chatLine = (g_wrapperChatCall != 0);
    if (g_lastShadowText[0]) {
        // The record has to come from the shadow pass that has just run. The chat renders
        // into a surface and the no-shadow state draws nothing at all, so a record left
        // from the frame before would hand the first line of a re-render a shadow it no
        // longer has - which is exactly the "F7 does nothing" the log used to show.
        const LONG age = (LONG)g_fontCallSeq - (LONG)g_lastShadowSeq;
        // The client paints the shadow with the {RRGGBB} tags already removed
        // (ChatStripColorTags), so the body has to be reduced the same way before the two
        // can be compared. Comparing the tagged body matched only the untagged lines - the
        // timestamp got its ring and every coloured message looked shadowless, which is why
        // F7 had nothing to switch.
        char plain[96];
        StripChatColourTags(text, n, plain, sizeof(plain));
        // The record is consumed, not timed. The chat renders into a surface: after F7 the
        // client re-renders once, and a time based window would still see the record left by
        // the previous frame and keep the shadow. Consuming it means a line that arrives
        // without a shadow in front of it has none here either.
        // The window used to be two calls, which only holds when the body follows the last
        // shadow copy with nothing in between. A chat line draws four to eight of those copies,
        // and the timestamp, another line or an F7 re-render between them pushed the body past
        // the window: the line then got no outline here *and* the client's black copies stayed
        // on screen, which is the dark double image. The text still has to match, so widening
        // the call window (bounded in time) keeps the pairing exact and stops losing it.
        const bool recent = age >= 0 && age <= 64 &&
                            (::GetTickCount() - g_lastShadowTick) <= 200;
        if (recent && ::strcmp(plain, g_lastShadowText) == 0) {
            outline = 1;
            chatLine = true;
            g_lastShadowText[0] = 0;
            why = L"record consumed";
            ConfirmShadowPasses(plain);
        } else {
            why = (age < 0 || age > 64) ? L"stale record" : L"no match";
        }
    }

    std::wstring w = ToWide(text, n);
    if (w.empty()) return 0;

    // A control is drawn twice: a translucent black shadow pass, then the text in its state
    // colour. That shadow pass is taken over as if it were text unless it is recognised -
    // and drawing it put an opaque ring around every button label, which is what made them
    // look dark. A translucent near-black colour is the client's shadow, so it is dropped.
    {
        const DWORD a = (colour >> 24) & 0xFF;
        if (a > 0 && a < 0x80 && (colour & 0x00FFFFFFu) < 0x101010u) {
            // Remember that this caller's line has a shadow, so the body that follows can
            // put one copy back instead of the pass we just dropped.
            if (g_wrapperRetRva != 0xFFFFFFFFu) {
                g_controlShadowRva  = g_wrapperRetRva;
                g_controlShadowTick = ::GetTickCount();
            }
            return (INT)px;
        }
    }

    // The client draws a control twice with the same rectangle and colour, so taking both
    // passes over drew the label twice and it came out bolder and darker than the client's.
    {
        const DWORD now = ::GetTickCount();
        const bool same = (now - g_lastWrapperTick) < 40 && colour == g_lastWrapperColour &&
                          w == g_lastWrapperText &&
                          g_lastWrapperRect.left == rect->left &&
                          g_lastWrapperRect.top == rect->top &&
                          g_lastWrapperRect.right == rect->right;
        g_lastWrapperText  = w;
        g_lastWrapperRect  = *rect;
        g_lastWrapperColour = colour;
        g_lastWrapperTick  = now;
        if (same) return (INT)px;
    }

    // Inside a panel's draw the fill would cover anything painted now, so the line is
    // recorded and painted when that draw has finished.
    if (g_inPanelDraw) {
        PendingText item;
        item.text   = w;
        item.l = rect->left; item.t = rect->top; item.r = rect->right; item.b = rect->bottom;
        item.colour = colour;
        item.format = (long)format;
        item.px     = px;
        item.weight = (weight > 0) ? weight : (int)g_cfg.clientTextWeight;
        item.fontId = chatLine ? g_cfg.clientTextChatFont : g_cfg.clientTextFont;
        item.scaleX = 1.0f;   // never scaled: the client computed the next piece's x from its own metrics
        // Same rule as the immediate path: a copy only when this caller's shadow pass was
        // dropped for it, which an edit box never has.
        item.copies = (g_shadowCopies >= 0)
                          ? g_shadowCopies
                          : ((g_controlShadowRva == g_wrapperRetRva &&
                              (::GetTickCount() - g_controlShadowTick) < 100) ? 1 : 0);
        PushPendingText(item);
        return (INT)px;
    }

    if (g_cfg.verbose) {
        static DWORD s_lastDraw = 0;
        const DWORD now = ::GetTickCount();
        if (now - s_lastDraw >= 200) {
            s_lastDraw = now;
            LogW(L"draw: ours src=%s px=%d outline=%d weight=%d face=%d colour=%08X \"%S\"",
                 g_drawSource, px,
                 outline, (weight > 0) ? weight : (int)g_cfg.clientTextWeight,
                 g_wrapperChatCall ? g_cfg.clientTextChatFont : g_cfg.clientTextFont,
                 (unsigned)colour, text);
        }
    }

    // How much shadow the line gets, in copies of itself. The client paints four copies in
    // a cross up to 1280 px wide and eight above; client_text_chat_shadow selects that (2),
    // the four-copy cross alone (1) or none (0). A chat line's shadow comes from the pairing
    // above, so F7 - which makes the client draw no shadow pass - takes it away entirely.
    const int chatShadow = (int)g_cfg.clientTextChatShadow;
    // A chat line takes its shadow from the record; anything else here is a DXUT control
    // (a dialog title, a button caption) whose translucent shadow pass was dropped above,
    // so it gets one copy at the configured offset - exactly the shadow the client drew.
    // Only when that pass really was dropped for this caller: an edit box paints its text
    // once, and a copy under it darkened white glyphs.
    const bool controlShadow = !chatLine && g_controlShadowRva == g_wrapperRetRva &&
                               (::GetTickCount() - g_controlShadowTick) < 100;
    g_shadowCopies = outline ? ((chatShadow >= 2) ? 8 : (chatShadow <= 0 ? 0 : 4))
                             : (chatLine ? 0 : (controlShadow ? 1 : 0));
    // A chat line is drawn from CChat::Draw, whichever way the shadow ended up being
    // handled: it uses the face the client uses there, and Arial needs no horizontal
    // squeeze. Tying this to the shadow instead meant the chat fell back to the display
    // face and its width factor, so the glyphs came out ten percent narrow and the
    // client shadow showed beside them as a black copy.
    if (g_cfg.verbose && chatLine) {
        static DWORD s_last = 0;
        static int   s_lastOutline = -1;
        const DWORD now = ::GetTickCount();
        if (outline != s_lastOutline || now - s_last > 1000) {
            s_lastOutline = outline;
            s_last = now;
            LogW(L"chat line: outline=%d (%s) \"%S\"", outline, why, text);
        }
    }
    g_fontIdOverride = chatLine ? g_cfg.clientTextChatFont : -1;
    g_hscaleOverride = -1.0f;
    const LONG prevWeight = (LONG)::InterlockedExchange(&g_weightOverride, (LONG)weight);
    { const wchar_t* prevSrc = g_drawSource; g_drawSource = L"font";
    TakeOverClientText(0, (void*)nullptr, sprite, nullptr, rect->left, rect->top, rect->right,
                       rect->bottom, colour, (long)format, px, w.c_str());
    g_drawSource = prevSrc; }
    ::InterlockedExchange(&g_weightOverride, prevWeight);
    g_shadowCopies = -1;
    g_fontIdOverride = -1;
    g_hscaleOverride = -1.0f;
    return (INT)px;
}

static INT __stdcall WrapperDrawTextADetour(void* self, void* sprite, const char* text,
                                            int count, RECT* rect, DWORD format,
                                            D3DCOLOR colour)
{
    // The chat is the only wrapper caller that draws a line with a shadow the client
    // itself would have drawn. Two ways to know it: the per-build window, or a body site
    // learned from matching a shadow record - the second is what makes an unmeasured
    // build work without a new table entry.
    {
        const BYTE* ret = (const BYTE*)_ReturnAddress();
        const BYTE* base = (const BYTE*)g_sampBase;
        const DWORD rva = base ? (DWORD)(ret - base) : 0xFFFFFFFFu;
        const bool fromChat = g_chatRenderHooked
                                  ? InChatRender()
                                  : (InWindow(g_chatWindow, rva) || ChatBodySiteKnown(rva));
        ::InterlockedExchange(&g_wrapperChatCall, fromChat ? 1 : 0);
        g_wrapperRetRva = rva;
    }
    __try {
        return WrapperTextBody(self, sprite, text, count, rect, format, colour);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_wrapperOrigA)
            return g_wrapperOrigA(self, sprite, text, count, rect, format, colour);
        return 0;
    }
}

typedef INT(__stdcall* WrapperDrawTextWFn)(void* self, void* sprite, const wchar_t* text,
                                           int count, RECT* rect, DWORD format,
                                           D3DCOLOR colour);
static WrapperDrawTextWFn g_wrapperOrigW = nullptr;

// px and weight of the font the wrapper draws with, read from the ID3DXFont it holds.
static bool WrapperFontMetrics(void* self, int* px, int* weight)
{
    if (px) *px = 0;
    if (weight) *weight = -1;
    ID3DXFont* inner = self ? *(ID3DXFont**)((BYTE*)self + 4) : nullptr;
    if (!inner) return false;
    FontGetDescAFn getDesc = (FontGetDescAFn)(*(void***)inner)[4];
    if (!getDesc) return false;
    D3DXFONT_DESCA desc = {};
    if (FAILED(getDesc(inner, &desc)) || desc.Height < 6 || desc.Height > 96) return false;
    if (px) *px = desc.Height;
    if (weight && desc.Weight >= 100 && desc.Weight <= 900) *weight = (int)desc.Weight;
    return true;
}

// Same rule as ClientTextWanted, over wide text: anything the client's own face cannot
// draw is ours, and plain ASCII follows client_text_ascii.
static bool ClientTextWantedW(const wchar_t* text, int len)
{
    if (!g_cfg.clientText || !text || len <= 0) return false;
    for (int i = 0; i < len; i++)
        if ((unsigned)text[i] >= 0x80) return true;
    if (g_cfg.clientTextAsciiOurs) return true;
    return len > 8;
}

// Callers that asked this entry point for a *measurement* of wide text first. An edit box
// does exactly that - it measures the character under the caret - and then paints the line
// itself: scrolled to the first visible character, with a selection rectangle behind part of
// it and a caret after it, all from its own clip rectangle. Taking that line over left the
// text invisible until it was selected, because the highlight and the caret stayed where the
// client put them. A caller that measures before it draws is such a control, so its draws
// stay with the client; nothing else in this client measures wide text.
static DWORD g_measureCallers[8] = { 0 };
static int   g_measureCallerCount = 0;

static bool MeasuredCaller(DWORD rva)
{
    if (rva == 0 || rva == 0xFFFFFFFFu) return false;
    for (int i = 0; i < g_measureCallerCount; i++)
        if (g_measureCallers[i] == rva) return true;
    return false;
}

static void NoteMeasureCaller(DWORD rva)
{
    if (rva == 0 || rva == 0xFFFFFFFFu || MeasuredCaller(rva) || g_measureCallerCount >= 8)
        return;
    g_measureCallers[g_measureCallerCount++] = rva;
    LogW(L"wide text: caller +0x%X measures before it draws, its text stays with the client "
         L"(%d known)",
         (unsigned)rva, g_measureCallerCount);
}

// The one thing that identifies an edit box at this entry point is its caret measurement: a
// DT_CALCRECT call with a fully transparent colour and an empty rectangle, once per frame
// while the box is on screen. Nothing else in this client measures wide text that way. While
// one is being drawn, this entry point stays out of the way entirely - the box scrolls its
// text, places its caret and paints its selection from its own metrics, and any glyph this
// mod painted there would not line up with them.
// ---------------------------------------------------------------------------
// Editable text versus static text
//
// A dialog's title, its buttons and its list rows are painted through this entry point, and
// so is its input field - all from client code, all with the same flags. Static looks at
// flags and callers failed: the labels and the field come from the same functions, and the
// field's caret measurement is issued by yet another one.
//
// What actually differs is the text itself: a label never changes, while an editable field
// holds whatever the player has typed so far. So a (caller, rectangle) whose text has
// changed at least once is an editable field, and from then on that site is drawn by the
// client - it scrolls the text, places the caret and paints the selection from its own
// metrics, and any glyph painted here would not line up with them. Everything else is a
// label: it is drawn here, which is what puts a server's Chinese button caption or dialog
// title on screen at all.
// ---------------------------------------------------------------------------
struct WideSite { DWORD rva; long l, t; bool editable; wchar_t last[64]; DWORD tick; };
static WideSite g_wideSites[16] = {};
static int      g_wideSiteCount = 0;

static WideSite* FindWideSite(DWORD rva, const RECT* rect, bool create)
{
    for (int i = 0; i < g_wideSiteCount; i++) {
        WideSite& s = g_wideSites[i];
        if (s.rva == rva && s.l == rect->left && s.t == rect->top) return &s;
    }
    if (!create || g_wideSiteCount >= 16) return nullptr;
    WideSite& s = g_wideSites[g_wideSiteCount++];
    s.rva = rva; s.l = rect->left; s.t = rect->top;
    s.editable = false; s.last[0] = 0; s.tick = 0;
    return &s;
}

// Answer a DT_CALCRECT call with the metrics this mod draws with. A control positions its
// caret, its selection rectangle and its centring from this number, so it has to be this
// mod's number: forwarding the call to the client made an edit box line its highlight and
// caret up with the client's font while the glyphs came from ours, which is what put the
// chat input's text "behind" its own selection.
static INT AnswerMeasureW(const wchar_t* text, int n, int px, RECT* rect)
{
    int width = 0, height = px;
    if (text && n > 0 && px > 0) {
        const std::wstring w(text, (size_t)n);
        width  = MeasureTextWidth(w, px, g_cfg.clientTextFont);
        height = MeasureLineHeight(px, g_cfg.clientTextFont);
    }
    if (width <= 0) width = px / 2;
    if (height <= 0) height = px;
    rect->right  = rect->left + width;
    rect->bottom = rect->top + height;
    return height;
}

static INT __stdcall WrapperTextBodyW(void* self, void* sprite, const wchar_t* text,
                                      int count, RECT* rect, DWORD format, D3DCOLOR colour)
{
    if (!g_wrapperOrigW || !text || !rect)
        return g_wrapperOrigW ? g_wrapperOrigW(self, sprite, text, count, rect, format, colour) : 0;

    // The wide entry point is the DXUT controls: the dialog title, the buttons (including
    // the spawn screen's << >> and Spawn), the list rows' captions and the input boxes.
    // They used to be forwarded on purpose, which is why those labels kept the client's own
    // bitmap font while everything else was redrawn. client_text_wrapper_wide = off brings
    // that back for the one control that positions itself from its own clip rectangle.
    // DT_CALCRECT is a measurement: a control lays itself out from the answer, so the
    // client has to produce it. Taking one over drew nothing but its shadow copy - a small
    // black mark wherever the sprite transform put it, which is the artefact that appeared
    // at the top of the screen while the chat input was open.
    const bool measureW = (format & 0x0400) != 0;
    const DWORD wRva = g_wrapperRetRva;
    const int n = (count < 0) ? (int)::wcslen(text) : count;

    // Has this site's text changed? Then it is an editable field, not a label. An empty
    // string counts as the same evidence and is the earliest form of it: an input field is
    // drawn empty before the player types, and a button or title never draws nothing. So the
    // field is left to the client from the first frame, not from the first keystroke.
    bool editable = false;
    if (!measureW) {
        WideSite* site = FindWideSite(wRva, rect, true);
        if (site) {
            if (n <= 0) {
                if (!site->editable)
                    LogW(L"wide text: +0x%X at (%ld,%ld) draws nothing, treated as an input "
                         L"field", (unsigned)wRva, rect->left, rect->top);
                site->editable = true;
            } else if (text) {
                if (site->last[0] && ::wcscmp(site->last, text) != 0) {
                    if (!site->editable)
                        LogW(L"wide text: +0x%X at (%ld,%ld) holds editable text, left to the "
                             L"client", (unsigned)wRva, rect->left, rect->top);
                    site->editable = true;
                }
                ::wcsncpy_s(site->last, _countof(site->last), text, _TRUNCATE);
            }
            editable = site->editable;
        }
    }

    if (!g_cfg.clientText || !g_cfg.clientTextWrapper || !g_cfg.clientTextWrapperWide ||
        editable || !ClientTextWantedW(text, n)) {
        return g_wrapperOrigW(self, sprite, text, count, rect, format, colour);
    }

    int px = 0, weight = -1;
    if (!WrapperFontMetrics(self, &px, &weight))
        return g_wrapperOrigW(self, sprite, text, count, rect, format, colour);

    // A measurement is answered here, not forwarded: the caller lays itself out from this
    // number (caret, selection, centring), so it has to be the width this mod will draw.
    if (measureW) return AnswerMeasureW(text, n, px, rect);

    // A control is drawn twice: a translucent near-black shadow pass, then the text in its
    // state colour. Dropping the first is what keeps button labels from being doubled.
    {
        const DWORD a = (colour >> 24) & 0xFF;
        if (a > 0 && a < 0x80 && (colour & 0x00FFFFFFu) < 0x101010u) {
            // Remember that this caller's line has a shadow, so the body that follows can
            // put one copy back instead of the pass we just dropped.
            if (g_wrapperRetRva != 0xFFFFFFFFu) {
                g_controlShadowRva  = g_wrapperRetRva;
                g_controlShadowTick = ::GetTickCount();
            }
            return (INT)px;
        }
    }

    // The client draws a control twice with the same rectangle and colour, so taking both
    // passes over drew the label twice and it came out bolder than the client's.
    {
        const DWORD now = ::GetTickCount();
        const bool same = (now - g_lastWrapperTick) < 40 && colour == g_lastWrapperColour &&
                          text == g_lastWrapperText &&
                          g_lastWrapperRect.left == rect->left &&
                          g_lastWrapperRect.top == rect->top &&
                          g_lastWrapperRect.right == rect->right;
        g_lastWrapperText  = text;
        g_lastWrapperRect  = *rect;
        g_lastWrapperColour = colour;
        g_lastWrapperTick  = now;
        if (same) return (INT)px;
    }

    // Inside a panel's draw the fill would cover anything painted now, so the line is
    // recorded and painted when that draw has finished.
    if (g_inPanelDraw) {
        PendingText item;
        item.text   = std::wstring(text, (size_t)n);
        item.l = rect->left; item.t = rect->top; item.r = rect->right; item.b = rect->bottom;
        item.colour = colour;
        item.format = (long)format;
        item.px     = px;
        item.weight = (weight > 0) ? weight : (int)g_cfg.clientTextWeight;
        item.fontId = g_cfg.clientTextFont;
        item.scaleX = 1.0f;
        // Same rule as the immediate path: a copy only when this caller's shadow pass was
        // dropped for it, which an edit box never has.
        item.copies = (g_shadowCopies >= 0)
                          ? g_shadowCopies
                          : ((g_controlShadowRva == g_wrapperRetRva &&
                              (::GetTickCount() - g_controlShadowTick) < 100) ? 1 : 0);
        PushPendingText(item);
        return (INT)px;
    }

    const int    prevFace   = g_fontIdOverride;
    const float  prevHs     = g_hscaleOverride;
    const int    prevCopies = g_shadowCopies;
    // Same rule as the narrow entry point: one copy only for a caller whose translucent
    // shadow pass was thrown away above. The chat input never draws one, so it gets none -
    // an unconditional copy put black under its white text and darkened it.
    g_shadowCopies   = (g_controlShadowRva == g_wrapperRetRva &&
                        (::GetTickCount() - g_controlShadowTick) < 100) ? 1 : 0;
    g_fontIdOverride = g_cfg.clientTextFont;
    g_hscaleOverride = -1.0f;
    const LONG prevWeight = (LONG)::InterlockedExchange(&g_weightOverride, (LONG)weight);
    { const wchar_t* prevSrc = g_drawSource; g_drawSource = L"wrapper";
    TakeOverClientText(0, self, sprite, nullptr, rect->left, rect->top, rect->right,
                       rect->bottom, colour, (long)format, px, text);
    g_drawSource = prevSrc; }
    ::InterlockedExchange(&g_weightOverride, prevWeight);
    g_fontIdOverride = prevFace;
    g_hscaleOverride = prevHs;
    g_shadowCopies   = prevCopies;
    ::InterlockedIncrement(&g_ctLogged);
    return (INT)px;
}

static INT __stdcall WrapperDrawTextWDetour(void* self, void* sprite, const wchar_t* text,
                                            int count, RECT* rect, DWORD format,
                                            D3DCOLOR colour)
{
    // The caller of the client's own vtable slot, captured here and not inside the body:
    // _ReturnAddress() in the body would name this detour, which is in this module and has
    // no meaning as a client call site.
    g_wrapperRetRva = CallerRva((const BYTE*)_ReturnAddress());
    __try {
        return WrapperTextBodyW(self, sprite, text, count, rect, format, colour);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_wrapperOrigW)
            return g_wrapperOrigW(self, sprite, text, count, rect, format, colour);
        return 0;
    }
}

void RetryHookWrapperVtable()
{
    if (g_wrapperHooked || !g_sampBase || !g_cfg.clientText || !g_cfg.clientTextWrapper) return;

    BYTE* base = (BYTE*)g_sampBase;
    BYTE* text = nullptr; DWORD textSize = 0;
    if (!GetTextSection((HMODULE)g_sampBase, &text, &textSize)) return;

    // The vtable is found, not assumed: an eighteen slot table in .rdata whose slot 14
    // points at the function carrying the client's own 100000 character guard
    // (cmp eax, 0x186A0). Measured: R1 +0xD7FE0, R2 +0xD7FF0, R3 +0xEA3B8, R4 +0xEA410,
    // R5 +0xEA410, DL +0x11C438 - so the chat and the buttons are reached on every build.
    void** vt = nullptr;
    {
        static const BYTE kGuard[5] = { 0x3D, 0xA0, 0x86, 0x01, 0x00 };
        BYTE* rdata = nullptr; DWORD rdataSize = 0;
        if (!GetSectionByName((HMODULE)g_sampBase, ".rdata", &rdata, &rdataSize)) return;

        __try {
            for (DWORD off = 0; off + 72 <= rdataSize; off += 4) {
                DWORD* cand = (DWORD*)(rdata + off);
                bool allText = true;
                for (int i = 0; i < 18 && allText; i++) {
                    BYTE* fn = (BYTE*)(uintptr_t)cand[i];
                    if (!fn || fn < text || fn >= text + textSize) allText = false;
                }
                if (!allText) continue;
                BYTE* draw = (BYTE*)(uintptr_t)cand[14];
                bool guarded = false;
                for (int i = 0; i + 5 < 0x200; i++) {
                    if (::memcmp(draw + i, kGuard, sizeof(kGuard)) == 0) { guarded = true; break; }
                }
                if (guarded) { vt = (void**)cand; break; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            vt = nullptr;
        }
        if (!vt) {
            LogW(L"the CFontWrapper vtable signature was not found");
            return;
        }
        LogW(L"CFontWrapper vtable found at +0x%X", (unsigned)((BYTE*)vt - base));
    }
    // Validate before patching: all eighteen slots must live inside this module.
    __try {
        for (int i = 0; i < 18; i++) {
            BYTE* fn = (BYTE*)vt[i];
            if (!fn || fn < base || fn >= base + 0x200000) return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    DWORD old = 0;
    if (!::VirtualProtect(&vt[14], sizeof(void*), PAGE_READWRITE, &old)) return;
    g_wrapperOrigA = (WrapperDrawTextAFn)vt[14];
    if (!g_wrapperOrigA) {
        DWORD tmp = 0;
        ::VirtualProtect(&vt[14], sizeof(void*), old, &tmp);
        return;
    }
    g_wrapperOrigW = (WrapperDrawTextWFn)vt[15];
    FreezeThreads();
    vt[14] = (void*)&WrapperDrawTextADetour;
    if (g_wrapperOrigW) vt[15] = (void*)&WrapperDrawTextWDetour;
    UnfreezeThreads();
    DWORD tmp = 0;
    ::VirtualProtect(&vt[14], sizeof(void*), old, &tmp);

    g_wrapperHooked = true;
    LogW(L"CFontWrapper vtable hooked: DrawTextA (DXUT text, buttons, list rows, chat)");
}

void RetryHookFontVtable()
{
    if (g_fontVtHooked || !g_cfg.clientText || !g_cfg.clientTextFontLevel) return;

    IDirect3DDevice9* dev = RenderDevice();
    if (!dev) return;

    // Every loaded d3dx9 has its own vtable and the client's fonts may belong to any
    // of them, so each gets a probe font of its own.
    HMODULE mod = nullptr;
    for (int i = 43; i >= 24 && !mod; i--) {
        char dll[32];
        wsprintfA(dll, "d3dx9_%d.dll", i);
        mod = ::GetModuleHandleA(dll);
    }
    if (!mod) return;
    CreateFontAFn create = (CreateFontAFn)::GetProcAddress(mod, "D3DXCreateFontA");
    if (!create) return;

    // The client created its fonts long before this mod sees a frame, so instead of
    // waiting for one to appear, create one here: d3dx9 gives every font the same
    // vtable, so patching the vtable of this throwaway font patches the client's,
    // DXUT's and the chat's at the same time - and no samp.dll address is involved.
    int patched = 0;
    for (int i = 43; i >= 24; i--) {
        char dll[32];
        wsprintfA(dll, "d3dx9_%d.dll", i);
        HMODULE m = ::GetModuleHandleA(dll);
        if (!m) continue;
        CreateFontAFn fn = (CreateFontAFn)::GetProcAddress(m, "D3DXCreateFontA");
        if (!fn) continue;

        ID3DXFont* probe = nullptr;
        HRESULT hr = fn(dev, 8, 0, 0, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial", &probe);
        if (FAILED(hr) || !probe) continue;

        const bool wasHooked = g_fontVtHooked;
        g_fontVtHooked = false;
        PatchFontVtable(probe);
        g_fontVtHooked = true;
        patched++;

        if (!wasHooked) {
            // The self-test draws one glyph to see whether the detour runs, but a visible
            // draw at the top left corner is not worth a diagnostic: the call is made at a
            // position far outside the frame, so nothing lands on screen.
            const LONG before = g_fontPassed + g_fontDropped + g_fontTaken;
            RECT rc = { -4000, -4000, -3960, -3980 };
            FontDrawTextAFn call = (FontDrawTextAFn)(*(void***)probe)[14];
            if (call) call(probe, nullptr, "x", 1, &rc, 0, 0xFFFFFFFFu);
            const LONG after = g_fontPassed + g_fontDropped + g_fontTaken;
            LogW(L"font hook self-test via %S: %s", dll,
                 after > before ? L"the detour runs" : L"NOT called");
        }

        typedef ULONG(STDMETHODCALLTYPE* ReleaseFn)(ID3DXFont*);
        ReleaseFn rel = (ReleaseFn)(*(void***)probe)[2];
        if (rel) rel(probe);
    }
    if (!patched) return;

}


// ---------------------------------------------------------------------------
static void TakeOverClientText(int which, void* self, void* sprite, const char* text,
                               long l, long t, long r, long b, DWORD colour, long format,
                               int forcePx, const wchar_t* wideText);

static int FontTextCommon(bool wide, ID3DXFont* self, ID3DXSprite* sprite, const void* raw,
                          INT count, RECT* rect, DWORD format, D3DCOLOR colour,
                          const BYTE* retAddr)
{
    FontDrawTextAFn origA = g_fontOrigA;
    FontDrawTextWFn origW = g_fontOrigW;
    if (!origA || !origW) return 0;
    const LONG callSeq = ::InterlockedIncrement(&g_fontCallSeq);

    // This level draws nothing but the death window's body. It removes the client's own
    // black passes where this mod draws the line itself: the chat's shadow and the death
    // window's four outline copies.
    //
    // Measuring calls are never touched: they are black by convention and the client lays
    // its own lines out from the answer, so dropping one would move a whole row.
    const bool measure = (format & 0x0400) != 0;
    // A one glyph string in black is a plate, not a shadow: the death window paints its
    // weapon background that way, and dropping it took the black plate away.
    int glyphs = 0;
    if (raw) {
        if (wide) glyphs = (int)wcslen((const wchar_t*)raw);
        else      glyphs = (int)::strnlen((const char*)raw, 64);
    }

    // Measured: the chat's lines come from +0x6722A, its timestamp shadow from +0x66FA7
    // and the death window's name from +0x69F2E.
    const DWORD callerRva = retAddr ? CallerRva(retAddr) : 0xFFFFFFFFu;
    const bool chatCall  = g_chatRenderHooked ? InChatRender()
                                              : InWindow(g_chatWindow, callerRva);
    const bool deathCall = InWindow(g_deathWindow, callerRva);

    // CDeathWindow::DrawText paints four black copies of a name and then the body, and
    // Draw() measures each name once before that (DT_CALCRECT, white). Only the drawing
    // calls are counted: the fifth is the body. The body can be black too (a victim's name
    // can be), so the counter, not the colour, is what tells it from an outline pass.
    static const void* s_deathFont = nullptr;
    static char        s_deathText[64] = { 0 };
    static int         s_deathCount = 0;
    int deathPass = 0;                                     // 1..4 outline, 5 body
    if (deathCall && rect && glyphs > 1 && !measure) {
        char head[64] = { 0 };
        if (!wide && raw) {
            int i = 0;
            for (; i < (int)sizeof(head) - 1 && ((const char*)raw)[i]; i++) head[i] = ((const char*)raw)[i];
            head[i] = 0;
        }
        if (s_deathFont != (const void*)self || ::strcmp(head, s_deathText) != 0) {
            s_deathFont = (const void*)self;
            ::strcpy_s(s_deathText, sizeof(s_deathText), head);
            s_deathCount = 0;
        }
        ++s_deathCount;
        deathPass = ((s_deathCount - 1) % 5) + 1;
    }

    const bool blackPass = (colour & 0x00FFFFFFu) == 0;
    // glyphs >= 1: a one character chat message draws its shadow as eight copies of that
    // single character, and requiring more than one left those passes with the client - the
    // black smear that only a one character message produced.
    //
    // One glyph is excluded inside the death window, though: its weapon plate is a single
    // black glyph (a filled rounded square) drawn twice before the white weapon icon, and
    // treating that as a shadow pass removed the plate and left the icons floating on the
    // ground. The names in the same window are always longer than one character.
    const bool drawable  = !measure && sprite && (glyphs > 1 || (glyphs == 1 && !deathCall));
    // Only when this mod is the one drawing the line: with client_text = off the client
    // keeps its own glyphs, and dropping its passes then left the chat shadowless with
    // nothing of ours to take their place.
    const bool dropPasses = g_cfg.clientTextDropShadows && g_cfg.clientText == 2;

    // The chat: the dropped pass is remembered, and the body that follows reads it back
    // to decide whether the line carries an outline (see WrapperTextBody).
    // A black copy of a line is a shadow pass. Its caller is remembered together with the
    // text, so when this mod sees the chat body for that text it learns the site that drew
    // it and drops it from then on. That is how a build nobody measured ends up correct
    // after its first line instead of needing a table entry per version.
    if (dropPasses && drawable && raw && !wide && blackPass) {
        const char* cs = (const char*)raw;
        const DWORD rva = CallerRva(retAddr);
        RememberShadowPass(self, rva, cs);
        // The record is written even when the pass itself is drawn, so a body whose site is
        // not learned yet still knows its line had a shadow.
        int n = 0;
        while (n < (int)sizeof(g_lastShadowText) - 1 && cs[n]) { g_lastShadowText[n] = cs[n]; n++; }
        g_lastShadowText[n] = 0;
        g_lastShadowTick = ::GetTickCount();
        g_lastShadowSeq = callSeq;
        if (chatCall || deathCall || ShadowSiteKnown(rva)) {
            ::InterlockedIncrement(&g_fontDropped);
            LogFontPass(L"drop", retAddr, self, cs, glyphs, rect, format, colour);
            return 0;
        }
    }

    // The death window: its four black passes are a full copy of the name, so leaving
    // them in place is what drew the black text beside ours. Dropping them and drawing the
    // body with this mod's own outline leaves one name; the weapon icon is a single glyph
    // and is never taken over, so it still lands where the client put it.
    if (dropPasses && drawable && deathCall && deathPass >= 1 && deathPass <= 4 && blackPass) {
        ::InterlockedIncrement(&g_fontDropped);
        LogFontPass(L"drop", retAddr, self, (!wide && raw) ? (const char*)raw : "", glyphs,
                    rect, format, colour);
        return 0;
    }

    // The death window's body is the coloured draw that follows its four black passes. On
    // a build whose callers were never measured that pairing is the only way to know it,
    // so the same rule applies: a coloured draw whose font and text match shadow passes
    // from the last moment is the body of a shadowed line, and this mod draws it with its
    // own outline once those passes have been learned and dropped. That is what makes a
    // 0.3.7 client look like the 0.3.DL one here without a table entry for it.
    const bool shadowedBody = !deathCall && !blackPass && raw && !wide && rect &&
                              g_cfg.clientTextDropShadows && g_cfg.clientText == 2 &&
                              ShadowPassRecent(self, (const char*)raw);
    if (((deathCall && deathPass == 5) || shadowedBody) && drawable && rect &&
        g_cfg.clientText == 2) {
        // A body found by the pairing also teaches the addresses: the passes it follows
        // are shadow sites from now on, so the next frame drops them without waiting.
        if (shadowedBody) ConfirmShadowPasses((const char*)raw);
        int px = 0;
        D3DXFONT_DESCA desc = {};
        FontGetDescAFn getDesc = (FontGetDescAFn)(*(void***)self)[4];
        if (getDesc && SUCCEEDED(getDesc(self, &desc)) && desc.Height >= 6 && desc.Height <= 96)
            px = desc.Height;

        if (px > 0) {
            const int n = (count < 0) ? (wide ? (int)wcslen((const wchar_t*)raw)
                                              : SafeStrLen((const char*)raw, 8192))
                                      : count;
            if (n > 0) {
                std::wstring w = wide ? std::wstring((const wchar_t*)raw, (size_t)n)
                                      : ToWide((const char*)raw, n);
                if (!w.empty()) {
                    g_fontIdOverride = -1;
                    g_hscaleOverride = -1.0f;
                    // The four passes that made the outline are gone when they were dropped
                    // here, so the body has to bring its own.
                    g_shadowCopies = dropPasses ? 4 : 0;
                    { const wchar_t* prevSrc = g_drawSource; g_drawSource = L"panel";
                    TakeOverClientText(0, (void*)nullptr, nullptr, nullptr, rect->left, rect->top,
                                       rect->right, rect->bottom, colour, (long)format, px,
                                       w.c_str());
                    g_drawSource = prevSrc; }
                    g_shadowCopies = -1;
                    ::InterlockedIncrement(&g_fontTaken);
                    LogFontPass(L"take", retAddr, self, (!wide && raw) ? (const char*)raw : "",
                                glyphs, rect, format, colour);
                    return px;
                }
            }
        }
    }

    LogFontPass(L"pass", retAddr, self, (!wide && raw) ? (const char*)raw : "", glyphs, rect,
                format, colour);
    ::InterlockedIncrement(&g_fontPassed);
    return wide ? origW(self, sprite, (const wchar_t*)raw, count, rect, format, colour)
                : origA(self, sprite, (const char*)raw, count, rect, format, colour);
}

static INT STDMETHODCALLTYPE FontDrawTextADetour(ID3DXFont* self, ID3DXSprite* sprite,
                                                 const char* text, INT count, RECT* rect,
                                                 DWORD format, D3DCOLOR colour)
{
    return FontTextCommon(false, self, sprite, (const void*)text, count, rect, format, colour,
                          (const BYTE*)_ReturnAddress());
}

static INT STDMETHODCALLTYPE FontDrawTextWDetour(ID3DXFont* self, ID3DXSprite* sprite,
                                                 const wchar_t* text, INT count, RECT* rect,
                                                 DWORD format, D3DCOLOR colour)
{
    return FontTextCommon(true, self, sprite, (const void*)text, count, rect, format, colour,
                          (const BYTE*)_ReturnAddress());
}




// Text drawn through a sprite may be positioned and scaled by that sprite's transform -
// the 3D labels are: their rect is computed in screen space but the sprite carries the
// projection scale. Without it the line lands at the wrong size and place.
struct SpriteXform { bool valid; float sx, sy, tx, ty; };

static SpriteXform GetSpriteXform(void* sprite)
{
    SpriteXform out = { false, 1.0f, 1.0f, 0.0f, 0.0f };
    if (!sprite) return out;

    typedef HRESULT(STDMETHODCALLTYPE* GetTransformFn)(void*, float*);
    GetTransformFn fn = (GetTransformFn)(*(void***)sprite)[4];   // ID3DXSprite::GetTransform
    if (!fn) return out;

    float m[16] = { 0 };
    __try {
        if (FAILED(fn(sprite, m))) return out;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return out;
    }

    // Only a plain scale/translate is understood; anything else is left alone.
    if (!(m[0] > 0.02f && m[0] < 64.0f) || !(m[5] > 0.02f && m[5] < 64.0f)) return out;
    if (!(m[12] > -20000.0f && m[12] < 20000.0f) || !(m[13] > -20000.0f && m[13] < 20000.0f))
        return out;

    out.valid = true;
    out.sx = m[0]; out.sy = m[5]; out.tx = m[12]; out.ty = m[13];
    return out;
}

// Replaces one line of the client's own UI text with this font. The client gives us
// everything a takeover needs: the box, the DT_* format, the colour and - in its own
// CFonts object - the font height it is about to use.
static void TakeOverClientText(int which, void* self, void* sprite, const char* text,
                               long l, long t, long r, long b, DWORD colour, long format,
                               int forcePx, const wchar_t* wideText)
{


    // Fully transparent: the client drew nothing, so neither may this - and drawing its
    // shadow copies while the body was invisible left a black mark on screen that no
    // body ever covered.
    if (((colour >> 24) & 0xFF) == 0) return;

    int px = forcePx;
    if (px <= 0) px = self ? *(int*)((BYTE*)self + (which ? 36 : 32)) : 0;
    if (px <= 0) px = 20;

    // A sprite may carry the projection scale the client draws a line with.
    const SpriteXform xf = GetSpriteXform(sprite);
    if (xf.valid) {
        px = (int)((float)px * xf.sy + 0.5f);
        l += (long)xf.tx;
        r += (long)xf.tx;
        t += (long)xf.ty;
        b += (long)xf.ty;
    }
    if (px < 6)   px = 6;
    if (px > 200) px = 200;

    // px is used as given: the client sized its layout for this exact pixel height
    if (px < 6)   px = 6;
    if (px > 200) px = 200;

    // A one pixel box is an anchor, not a layout: the chat draws that way, centred on
    // the point (DT_CENTER). Rebuild a box around it so the line lands where the client
    // would have put it.
    //
    // Only an exact one pixel box. The death window hands over an *inverted* box for its
    // right-aligned killer name (left = slot end - measured width, right = the next
    // column), and widening that one moved the name 600px to the left of the icon.
    if (r - l == 1) {
        const int wide = 1200;
        const long cx = l;
        l = cx - wide / 2;
        r = l + wide;
    }
    // An inverted box carries only its left edge: the client draws with DT_NOCLIP and
    // ignores the right edge, so the line starts there and is not clipped by us either.
    if (r <= l) r = l + 4096;
    if (b - t <= 1) b = t + (long)((float)px * 1.3f + 0.5f);

    const int boxW = (int)(r - l);
    const int boxH = (int)(b - t);
    if (boxW <= 1 || boxH <= 1) return;

    std::wstring w;
    if (wideText) {
        w.assign(wideText);
    } else {
        const int len = SafeStrLen(text, 4096);
        if (len <= 0) return;
        w = ToWide(text, len);
    }
    if (w.empty()) return;

    // D3DX lays consecutive lines one font height apart; 1.25 pushed the last lines of
    // the nine-line help dialog under its button.
    // While a panel draws, its translucent fill is painted after this line, so the line
    // is recorded and painted when that draw has returned.
    if (g_inPanelDraw) {
        PendingText q;
        q.text   = w;
        q.l = l; q.t = t; q.r = r; q.b = b;
        q.colour = colour;
        q.format = (long)format;
        q.px     = px;
        q.weight = (g_weightOverride > 0) ? (int)g_weightOverride : (int)g_cfg.clientTextWeight;
        q.fontId = (g_fontIdOverride >= 0) ? g_fontIdOverride : g_cfg.clientTextFont;
        q.scaleX = 1.0f;
        q.copies = (g_shadowCopies >= 0) ? g_shadowCopies : 0;
        PushPendingText(q);
        g_ctLogged++;
        return;
    }

    const int lineH = (int)((float)px * g_cfg.clientTextLine + 0.5f);

    int align = 0;
    if (format & 0x0001) align = 1;        // DT_CENTER
    else if (format & 0x0002) align = 2;   // DT_RIGHT

    int lines = 1;
    for (size_t i = 0; i < w.size(); i++) if (w[i] == L'\n') lines++;

    // D3DX centres or bottoms a single line inside the box; DXUT buttons and the
    // list box rows ask for that, and without it their text sticks to the top.
    int yOff = 0;
    if (format & 0x0004)      yOff = (boxH - lineH * lines) / 2;   // DT_VCENTER
    else if (format & 0x0008) yOff = boxH - lineH * lines;         // DT_BOTTOM
    if (yOff < 0) yOff = 0;



    static int s_group = 0;
    std::vector<DrawItem> items;
    size_t pos = 0;
    for (int line = 0; line < lines && pos <= w.size(); line++) {
        size_t eol = w.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = w.size();
        std::wstring one = w.substr(pos, eol - pos);
        pos = eol + 1;

        // The client asks for DT_EXPANDTABS on the dialogs; the tab stops lived in the
        // renderer that is skipped, so they are expanded here.
        std::wstring expanded;
        for (size_t i = 0; i < one.size(); i++) {
            if (one[i] == L'\t') {
                for (int k = 0; k < g_cfg.clientTextTab; k++) expanded.push_back(L' ');
            }
            else expanded.push_back(one[i]);
        }
        if (expanded.empty()) continue;

        const int group = ++s_group;
        std::vector<TextRun> runs = ParseColorRuns(expanded, colour, g_cfg.clientTextBraces);
        for (size_t ri = 0; ri < runs.size(); ri++) {
            if (runs[ri].text.empty()) continue;
            DrawItem it;
            it.forceOpaque = true;   // the client's own UI text is drawn with color | 0xff000000
            it.text        = runs[ri].text;
            // The client draws "[hh:mm:ss]" and then the message as a second call, positioning the
            // message with Arial's advance for the prefix. Our face is narrower than Arial, so the
            // prefix ends short of that position and a gap opens between the two - which is why the
            // join used to exist. It cannot be applied to every piece: a colour code in the middle
            // of a sentence also arrives as a second call, and pulling that one back ate the space
            // ("Connected toopen.mp"). Only a timestamp-shaped prefix gets joined.
            // The chat keeps the client's x per piece and shifts it by the accumulated difference
            // between this mod's advances and the client's; anything else uses its x as given.
            it.x           = ChatPiece()
                                 ? ChatOffsetX(t + (long)(line * lineH), (float)l, runs[ri].text, px,
                                               g_cfg.clientTextChatFont)
                                 : (float)l;
            // The cell this renderer produces carries more space below the ink than the
            // client's does, so a line centred by its cell sits low. Lift the ink a little.
            const int inkLift = (px + 6) / 12;
            it.y          = (float)(t + yOff - inkLift) + (float)(line * lineH);
            it.px          = px;
            it.fontId      = (g_fontIdOverride >= 0) ? g_fontIdOverride : g_cfg.clientTextFont;
            it.color       = runs[ri].color;
            // How many copies of the line the shadow has. The chat, the death window and
            // every CFonts caller answer this through g_shadowCopies, because the client
            // told them; what is left is the dialog text and the DXUT captions, and the
            // client paints those with no shadow at all - "auto" therefore means none, and
            // only an explicit client_text_outline = on gives them one.
            const int copies = (g_shadowCopies >= 0)
                                   ? g_shadowCopies
                                   : ((g_cfg.clientTextOutline == 1 && px >= 18) ? 4 : 0);
            it.outlineSize  = 0;
            it.shadowCopies = copies;
            it.color2       = ((DWORD)g_cfg.clientTextShadowAlpha << 24);
            it.shadowSize   = 0;
            // The copies are the shadow. DrawOneItem's [font] fallback would add one more at
            // (1,1) on top, which the client's four-copy cross does not have, so it is
            // switched off for every line this mod draws itself.
            it.shadowColor  = 0u;
            it.group       = group;
            it.scaleX      = (g_hscaleOverride > 0.0f) ? g_hscaleOverride
                                                       : 1.0f;
            it.weight      = (g_weightOverride > 0) ? (int)g_weightOverride
                                                     : g_cfg.clientTextWeight;
            if (ri == 0) { it.align = align; it.boxW = boxW; }
            items.push_back(it);
        }
    }
    if (items.empty()) return;

    // Drawn here, from inside the client's own call: its panels are painted inside
    // Present, after every hook this mod owns, so a later pass would be underneath.
    DrawClientTextNow(items);
    g_ctLogged++;
}

static void __fastcall ClientTextDetour0(void* self, void* unused, void* sprite,
                                         const char* text, long l, long t, long r, long b,
                                         DWORD color, int shadow)
{
    const DWORD caller = (DWORD)::_ReturnAddress();
    if (g_cfg.clientText >= 1) LogClientText(0, self, text, l, t, r, b, color, 0x120, caller);
    if (g_cfg.clientText == 2 && ClientTextWanted(text, SafeStrLen(text, 4096))) {
        // The client says whether this line carries a shadow, and that is what decides
        // whether ours does too: the scoreboard's rows ask for none, and a shadow drawn
        // anyway is what made them look bolder than the original.
        const int prev = g_shadowCopies;
        g_shadowCopies = shadow ? 4 : 0;
        TakeOverClientText(0, self, sprite, text, l, t, r, b, color, 0x120, 0, nullptr);
        g_shadowCopies = prev;
        return;                           // the client's own glyphs are not drawn
    }
    if (g_ctTramp[0]) {
        ((CFontsDrawTextFn)g_ctTramp[0])(self, nullptr, sprite, text, l, t, r, b,
                                         color, shadow);
    }
}

static void __fastcall ClientTextDetour1(void* self, void* unused, void* sprite,
                                         const char* text, long l, long t, long r, long b,
                                         int format, DWORD color, int shadow)
{
    const DWORD caller = (DWORD)::_ReturnAddress();
    if (g_cfg.clientText >= 1) LogClientText(1, self, text, l, t, r, b, color, format, caller);
    if (g_cfg.clientText == 2 && ClientTextWanted(text, SafeStrLen(text, 4096))) {
        const int prev = g_shadowCopies;
        g_shadowCopies = shadow ? 4 : 0;
        TakeOverClientText(1, self, sprite, text, l, t, r, b, color, format, 0, nullptr);
        g_shadowCopies = prev;
        return;
    }
    if (g_ctTramp[1]) {
        ((CFontsDrawLittleFn)g_ctTramp[1])(self, nullptr, sprite, text, l, t, r, b,
                                           format, color, shadow);
    }
}


void UnhookAll()
{
    RenderUnhookDevice();   // device slots first: the render thread may still call them

    if (g_tdHooked && g_tdThunk) {
        FreezeThreads();
        WriteMemory(g_tdThunk + samp::kThunkTextJmpOp, &g_tdOrigRel32, 4);
        UnfreezeThreads();
        g_tdHooked = false;
    }
    if (g_gtHooked && g_gtTarget) {
        FreezeThreads();
        WriteMemory(g_gtTarget, g_gtOrigBytes, samp::kGameTextProlog);
        UnfreezeThreads();
        g_gtHooked = false;
    }
    if (g_menuHooked && g_menuTarget) {
        FreezeThreads();
        WriteMemory(g_menuTarget, g_menuOrigBytes, 5);
        UnfreezeThreads();
        g_menuHooked = false;
    }
    if (g_menuShowHooked && g_menuShowTarget) {
        FreezeThreads();
        WriteMemory(g_menuShowTarget, g_menuShowOrigBytes, samp::kMenuShowProlog);
        UnfreezeThreads();
        g_menuShowHooked = false;
    }
    if (g_psHooked && g_psTarget) {
        FreezeThreads();
        WriteMemory(g_psTarget, g_psOrigBytes, 8);
    UnhookClientText();
        UnfreezeThreads();
        g_psHooked = false;
    }
}


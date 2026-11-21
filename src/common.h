// ---------------------------------------------------------------------------
// samp-unicode
// common.h - shared constants, configuration, draw items and module interfaces
// ---------------------------------------------------------------------------
#pragma once

// Bumped by hand when the public behaviour changes; printed at startup so a log
// always says which build produced it.
#define SAMP_UNICODE_VERSION "0.0.1"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <deque>
#include <cstring>
#include <cwctype>

// ---------------------------------------------------------------------------
// Constants verified against real samp.dll builds (see README "verification")
// ---------------------------------------------------------------------------
namespace samp {
    constexpr int kMenuShowProlog = 6;   // push ecx/ebx/ebp/esi + mov esi,ecx

// CTextDraw field offsets (identical across 0.3.7-R1..R5-1 and 0.3.DL;
// each one was cross-checked in the real CTextDraw::Draw disassembly)
constexpr DWORD kOffText        = 0x0000;   // char[0x321] text buffer (first copy)
constexpr DWORD kOffText2       = 0x0321;   // char[0x321] text buffer (game strcpy target)
constexpr DWORD kTextLen        = 0x321;    // buffer length including the terminating NUL
constexpr DWORD kOffLetterW     = 0x0963;   // float letter width
constexpr DWORD kOffLetterH     = 0x0967;   // float letter height
constexpr DWORD kOffColor       = 0x096B;   // DWORD text color (stored as AABBGGRR)
constexpr DWORD kOffBoxW        = 0x0972;   // float box width
constexpr DWORD kOffBoxH        = 0x0976;   // float box height
constexpr DWORD kOffColor2      = 0x097F;   // DWORD secondary color (shadow/outline)
constexpr DWORD kOffBoxFlag     = 0x0971;   // BYTE: box enabled
constexpr DWORD kOffBoxColor    = 0x097A;   // DWORD box colour (fed to CFont as the text background)
constexpr DWORD kOffFlagCenter  = 0x0970;   // BYTE
constexpr DWORD kOffFlagShadow  = 0x0983;   // BYTE
constexpr DWORD kOffFlagOutline = 0x0984;   // BYTE
constexpr DWORD kOffFlagRight   = 0x0986;   // BYTE
constexpr DWORD kOffFont        = 0x0987;   // DWORD: 0..3 text fonts, 4 sprite, 5 model preview
constexpr DWORD kOffX           = 0x098B;   // float X in the 640x448 virtual screen
constexpr DWORD kOffY           = 0x098F;   // float Y in the 640x448 virtual screen
constexpr DWORD kOffSpriteIdx   = 0x09A3;   // DWORD: sprite/preview slot, -1 for plain text
// Selection mode: the client writes these two itself (its own highlight pass,
// colour taken from SelectTextDraw), the server never does.
constexpr DWORD kOffHighlight      = 0x09D1;   // BYTE: this draw uses the highlight colour
constexpr DWORD kOffHighlightColor = 0x09D2;   // DWORD: ARGB passed to SelectTextDraw

// CTextDraw::Draw dispatch thunk (19 bytes, identical in all 7 builds):
//   83 B9 A3 09 00 00 FF      cmp dword ptr [ecx+0x9A3], -1
//   74 05                     je  +5
//   E9 xx xx xx xx            jmp <sprite / model preview renderer>
//   E9 xx xx xx xx            jmp <text renderer>
//
// +0x9A3 is an index into the client's sprite/preview table and is -1 for
// ordinary text draws, so the je taken path is the text renderer. Only that
// second jump is retargeted; sprite and model preview draws keep using the
// original code path untouched.
constexpr int kThunkLen          = 0x13;
constexpr int kThunkTextJmpOp    = 0x0F;   // operand of the second jmp (patched)
constexpr int kThunkTextJmpEnd   = 0x13;   // next instruction after it

// CGame::DisplayGameText prologue (10 bytes, identical in all 7 builds)
//   55                     push ebp
//   8B EC                  mov  ebp, esp
//   81 7D 10 C8 00 00 00   cmp  dword ptr [ebp+0x10], 0xC8   ; style > 200 -> return
constexpr int kGameTextProlog   = 10;

// GTA:SA 1.0 globals - the only host-process addresses this plugin relies on
constexpr DWORD kAddrD3DDevice  = 0x00C97C28;  // IDirect3DDevice9*
constexpr DWORD kAddrScreenW    = 0x00C17044;  // int
constexpr DWORD kAddrScreenH    = 0x00C17048;  // int

} // namespace samp

// ---------------------------------------------------------------------------
// Text fonts
//
// TextDrawFont 0..3 are four different faces in the original renderer (see the
// open.mp TextDrawFont page: 0 = blackletter, 1 = bold sans, 2 = wide all-caps,
// 3 = Pricedown). GameText styles pick one of them in their layout table.
// kFontDefault is the face used for everything a display face cannot draw -
// above all CJK, which none of the four GTA fonts carry.
// ---------------------------------------------------------------------------
constexpr int kFontRoleCount = 4;          // TextDrawFont 0..3
constexpr int kFontDefault   = 4;          // fallback face
constexpr int kFontFaceCount = 5;

extern std::wstring g_fontFaces[kFontFaceCount];

// ---------------------------------------------------------------------------
// Configuration ([font] / [gametext] / [encoding] / [debug])
//
// Key naming is our own: lowercase, underscore separated, units spelled out in
// the comments. Booleans accept on/off, true/false, yes/no and 1/0; ratios
// accept either a fraction (0.075) or a percentage (7.5%).
// ---------------------------------------------------------------------------
struct Config {
    // [font]
    std::wstring file;                  // file = 
    std::wstring family;                // family = 
    std::wstring face[kFontRoleCount];  // font0..font3: TextDrawFont 0..3 face (file or family)
    int   collectionIndex = 0;          // collection = 0 (index inside a .ttc)
    std::wstring fontFace;             // face = "" - family name for a .ttc with no name table
    float sizeDivisor     = 50.0f;      // size_divisor = 50 (client cell = 20H = letterH * screenH / 48)
    float baselineOffset  = 0.0f;       // baseline_offset = 0 (pixels)
    float glyphGamma      = 1.00f;      // glyph_gamma: 1 = GDI's own coverage (the client's look)
    bool  fontAntialias   = false;      // font.antialias: on = grayscale edges, off = the client's 1-bit stencil
    bool  outlineRaster   = false;      // font.outline_raster: GetGlyphOutline fill; unavailable on some
    int   shadowOffsetX   = 1;          // shadow_offset = 1, 1
    int   shadowOffsetY   = 1;
    bool  respectLetterWidth = true;   // letter_width = on
    bool  clientGlyphs    = true;       // client_glyphs = on (keep the client's punctuation symbols)
    bool  shadow          = true;       // shadow = on

    // [textdraw]
    DWORD fontMask        = 0x0Fu;     // fonts = 0, 1, 2, 3
    bool  braceColors     = false;     // parse {RRGGBB} / {RRGGBBAA}; the client does not
    bool  scoreboardUnder = true;      // scoreboard_layer = under | hide

    // [overlay]
    bool  hookEndScene    = false;      // hook_point = endscene (draw before Present overlays)

    // [paths]
    int   clientText      = 2;          // client_text = off | log | draw
                                        // at natural width, so squeezing it made ours narrower
                                        // than the original in the scoreboard and dialogs
    int   clientTextWeight = 400;       // client_text_weight: 400 normal, 700 bold
    int   clientTextShadowAlpha = 0;   // client_text_shadow_alpha: 0..255, 0 = the client's own
    bool  clientTextDropShadows = false; // client_text_drop_shadows: drop the client's passes
    int   clientTextFont   = 1;              // client_text_font = 1: Arial, the face the client
                                             // itself draws with; CJK falls back per glyph
    int   clientTextChatFont = 1;            // client_text_chat_font = 1: the chat uses Arial like the client
    int   clientTextChatShadow = 2;          // client_text_chat_shadow: 0 none, 1 the client's
                                         // four-copy cross, 2 its eight-copy pattern
    bool  clientTextAsciiOurs = false;       // client_text_ascii = ours: take plain ASCII over too
    bool  clientTextFontLevel = true;   // client_text_font_level: drop the client shadow passes
    bool  clientTextWrapper  = true;   // client_text_wrapper: DXUT text, buttons, rows, chat
    bool  clientTextWrapperWide = true; // client_text_wrapper_wide: the DrawTextW entry point, i.e.
                                       // dialog titles, buttons and the input boxes
    bool  clientTextBraces = true;      // client_text_braces = on ({RRGGBB} in chat and dialogs)
    int   clientTextTab    = 4;         // client_text_tab = 4 (spaces per tab)
    float clientTextLine   = 1.05f;     // client_text_line = 1.05 (line spacing, in font heights)
    int   clientTextOutline = -1;       // client_text_outline = auto (-1) | on | off

    // [menu]
    bool  menuEnabled     = true;      // take over native menu text
    int   menuFont        = 2;         // face for menu rows: 0..3, see [font]
    int   menuTitleFont   = 0;         // face for the menu title: the client draws it with face 0
    float menuTitleScale  = 1.0f;      // extra size for the title (1.0 = the size the game asks for)
    float menuTitleDy     = 0.0f;      // title offset in pixels, positive = down
    float menuHScale      = 1.0f;      // horizontal squeeze for menu text (1.0 = no change)
    float menuTitleWidth  = 0.80f;     // title_width = 0.80: glyph width vs a row glyph
    int   menuAlign       = 0;         // 0 = pen starts at x, 1 = centre, 2 = right
    float menuGlyph       = 1.0f;      // multiplier on the measured native glyph height
    int   menuOutline     = 1;         // rows: outline at the 20-px reference size
    int   menuShadow      = 3;         // rows: extra shadow (20-px reference, like the outline)
    int   menuTitleOutline = 2;        // title: outline at the 20-px reference size
    int   menuTitleShadow  = 0;        // title: extra shadow in pixels
    DWORD menuShadowColor  = 0x000000; // shadow_colour = 000000 (RRGGBB)
    int   menuShadowAlpha  = 255;      // shadow_alpha = 255 (0..255)
    DWORD menuColor       = 0;         // 0 = follow the game, else RRGGBB

    // [gametext]
    bool  gameTextEnabled = true;       // enabled = on
    DWORD gameTextIgnore  = 0;          // ignore_styles = none
    float gameTextScale   = 1.0f;       // scale = 1.0 (multiplies the per-style size)
    bool  gameTextFadeOut = true;       // fade_out = on: safe now that the background is restored first

    // [encoding]
    int   codePage        = 0;          // charset = auto
    bool  utf8First       = true;       // utf8_first = on
    bool  redrawAscii     = false;      // redraw_ascii = off
    bool  detect          = true;       // detect = on

    // [debug]
    bool  logEnabled      = true;       // log = on   (master switch for the log file)
    bool  verbose         = false;      // verbose = off (per-texture / per-quad lines)
};

extern Config       g_cfg;
extern std::wstring g_gameDir;      // with trailing backslash
extern std::wstring g_pluginStem;   // module name without extension
extern HMODULE      g_sampBase;

extern UINT         g_codePage;     // 0 = auto
extern bool         g_utf8First;
extern bool         g_detectCharset;

// ---------------------------------------------------------------------------
// Draw item / GameText queue entry
// ---------------------------------------------------------------------------
void SetAtlasWeight(int weight);   // texture.cpp: the weight the next glyph is drawn at

static const int kClientTextWeightDefault = 400;

struct DrawItem {
    bool  menu = false;               // from the menu takeover: a layer below the scoreboard
    int   align = 0;                  // 0 left, 1 centre, 2 right within boxW
    int   boxW  = 0;                  // the width the alignment is measured in
    int   group = 0;                  // non-zero: runs of one line, laid out in order
    std::wstring text;
    float    x = 0.0f, y = 0.0f;
    int      px = 12;
    int      weight = 400;            // rasterisation weight; only the client text sets it
    int      fontId = kFontDefault;   // TextDrawFont id 0..3, or the fallback face
    D3DCOLOR color  = 0xFFFFFFFF;
    D3DCOLOR color2 = 0xFF000000;      // outline
    D3DCOLOR shadowColor = 0xFF000000; // shadow, drawn under the outline
    int      shadowSize = 0;   // SA-MP shadow size in pixels (0 = off)
    int      outlineSize = 0;  // SA-MP outline thickness in pixels (0 = off)
    int      shadowCopies = 0; // client text: glyph copies the client itself would draw
                               // (4 in a cross, 8 on a screen wider than 1280)
    bool     forceOpaque = false;  // client UI text: the client's own code ends with "| 0xff000000"
    bool     center = false;
    bool     right  = false;
    float    scaleX = 1.0f;    // horizontal stretch from TextDrawLetterSize
    int      sprite = 0;       // 0 = text; 1..4 = client's own up/down/left/right
};

struct GameTextItem {
    std::wstring text;
    int   style = 1;      // 0..6, see the table in render.cpp
    DWORD start = 0;
    int   timeMs = 0;     // 0 = stays until it is replaced
};

// ---------------------------------------------------------------------------
// Module interfaces
// ---------------------------------------------------------------------------
// log.cpp
void LogOpen();
void LogDisable();   // close the log file and delete it (used when [debug] log = off)
void LogW(const wchar_t* fmt, ...);
void LogDbg(const wchar_t* fmt, ...);

// encoding.cpp
std::wstring ToWide(const char* s, int len = -1);           // UTF-8 strict -> configured code page
std::wstring ToWideCp(UINT cp, const char* s, int len = -1);
UINT         ResolveCodePage();
std::wstring CodePageName(UINT cp);
const wchar_t* ProbeStringForCp(UINT cp);                   // glyph probe sample for a code page

// util.cpp
std::wstring GetModulePath(HMODULE m);
std::wstring GetDirOf(const std::wstring& path);
std::wstring GetStemOf(const std::wstring& path);
DWORD GetImageTimestamp(HMODULE mod);                        // PE link time, UTC
bool  UtcStampToLocal(DWORD stamp, SYSTEMTIME* out);
bool         GetTextSection(HMODULE m, BYTE** base, DWORD* size);
bool         GetSectionByName(HMODULE m, const char* name, BYTE** base, DWORD* size);
BYTE*        FindPattern(BYTE* start, DWORD size, const BYTE* pat, const char* mask, int len);
bool         GetFileVersionWords(const wchar_t* path, WORD v[4], std::wstring* desc);
std::vector<std::wstring> SplitLines(const std::wstring& text);   // splits on '\n', drops '\r'
struct TextRun { std::wstring text; D3DCOLOR color; };
std::vector<TextRun> ParseColorRuns(const std::wstring& s, D3DCOLOR base,
                                    bool allowBrace = false);  // {RRGGBB} only when allowBrace

// Inline client symbols. ~u~ ~d~ ~<~ ~>~ are not glyphs: the client draws them
// from its own ButtonSprite textures (MODELS\PCBTNS.TXD). ParseColorRuns leaves
// private-use placeholders in the text, and the layout cuts every placeholder out
// into a piece of its own so it can be measured and drawn as a square sprite.
constexpr wchar_t kSymbolFirst = 0xE000;   // +0 up, +1 down, +2 left, +3 right
inline int SymbolSlot(wchar_t c)
{
    return (c >= kSymbolFirst && c <= kSymbolFirst + 3) ? (int)(c - kSymbolFirst) + 1 : 0;
}
// The client draws the symbol in a 17H square starting 2H below the pen, and the
// glyph box of a character is 20H tall, so both follow from the text height.
// The factors are calibrated against the client's own /tdglyphs capture: the
// sprite's ink has to come out as tall as a capital letter and sit about a
// tenth of the letter height lower, which is what the stock renderer produces.
// (17H/20H and 2H/20H describe the client's 20H glyph box, not our font cell.)
inline int SymbolSizePx(int pxHeight)   { const int v = (pxHeight * 85 + 50) / 100; return v < 4 ? 4 : v; }
inline int SymbolOffsetPx(int pxHeight) { return (pxHeight * 14 + 50) / 100; }
// What to draw when the client's texture cannot be read: the closest Unicode
// arrow, which is what the plugin used before it learned to read the sprites.
inline const wchar_t* ArrowFallback(int slot)
{
    switch (slot) {
    case 1:  return L"\u2191";
    case 2:  return L"\u2193";
    case 3:  return L"\u2190";
    default: return L"\u2192";
    }
}

// One drawable piece of a colour run: text for one face, or a client symbol.
struct DrawPiece { std::wstring text; int font = 0; int width = 0; int symbol = 0; };
void SplitRunIntoPieces(const std::wstring& text, int pxHeight, int fontId, std::vector<DrawPiece>& out);

// The client's own faces map part of ASCII punctuation into the symbol section of
// their glyph table. Returns the character to draw instead, or 0 for "the client
// draws nothing here".
wchar_t ClientGlyphFor(int fontId, wchar_t ch);

// config.cpp
void LoadConfig();          // read INI, write the default template when missing
int   IniGetInt(const wchar_t* key, int def);
float IniGetFloat(const wchar_t* key, float def);
std::wstring IniGetStr(const wchar_t* key, const wchar_t* def);

// font.cpp
bool LoadFont();            // sfnt/ttcf name table -> AddFontResourceExW -> glyph check
std::wstring ParseFontFamily(const std::wstring& file, int ttcIndex);

// texture.cpp
struct TexEntry {
    LPDIRECT3DTEXTURE9 tex = nullptr;
    int w = 0, h = 0;
    int logicalW = 0;   // the advance: measured width, without the texture's padding
};
// A stretch of text that one face can draw; see SplitByFont.
struct TextSpan { std::wstring text; int font; };

LPDIRECT3DTEXTURE9 GetTextTexture(IDirect3DDevice9* dev, const std::wstring& text, int pxHeight,
                                  int fontId, int* outW, int* outH,
                                  int* outLogicalW = nullptr);
void ClearTextureCache();
void LockTextureCache();       // recursive; render.cpp holds it while drawing
void UnlockTextureCache();
void SetTextureDevice(IDirect3DDevice9* dev);
int  MeasureTextWidth(const std::wstring& text, int pxHeight, int fontId);
int  MeasureLineHeight(int pxHeight, int fontId);
// Split a run into spans of one face each: characters the requested font has no
// glyph for (CJK in a Latin display face) go to the fallback face.
std::vector<TextSpan> SplitByFont(const std::wstring& s, int fontId);
// The client's own arrow textures (CFont::ButtonSprite[1..4], loaded from
// MODELS\PCBTNS.TXD). Returns null when the plugin cannot read them, in which
// case the renderer falls back to the Unicode arrow in the item's text.
LPDIRECT3DTEXTURE9 GetClientSymbolTexture(IDirect3DDevice9* dev, int symbol);
void ClearSymbolCache();   // drop the probed arrows (device change / reset)

// render.cpp
void RenderInit();
bool StopRequested();   // the module is being unloaded
void RenderUnhookDevice();
extern bool g_overlayReady;
void RenderOnOverlay(IDirect3DDevice9* dev);   // called from the frame hook
IDirect3DDevice9* RenderDevice();
void DrawUnderPanel(IDirect3DDevice9* dev);   // scoreboard detour: GameText + menu
void RenderOnReset();
bool RenderHookDevice(IDirect3DDevice9* dev);
void PushDrawItem(const DrawItem& item);
void PushGameText(const std::wstring& text, int style, int timeMs);
void ClearGameTextAll();              // drop everything we show (new message replaces)
bool IsGameTextText(const std::wstring& s);   // already drawn by the GameText path
void NoteMenuDraw(float x, float y, int px);  // remember what the menu path drew
bool IsMenuDuplicate(float x, float y, int px);
void NoteMenuFrame();                 // the menu drew a line this frame
bool MenuDrawnThisFrame();            // consumes that flag
int  GameTextStyleCount();          // styles with a built-in layout (0..N-1)
extern IDirect3DDevice9* g_device;

// hooks.cpp
bool HookTextDraw();
bool HookGameText();
bool HookScoreboardDraw();
bool HookDialogDraw();
bool HookDialogOnRender();      // CDXUTDialog::OnRender call sites: captions after the batch
bool HookMenuShow();   // CMenu::Show: tells the takeover a menu is on screen

// font.cpp
void FontsUnload();    // undo every AddFontResourceExW

// hooks.cpp
bool HookMenu();
bool HookPrintString();
void SelectClientCallerWindows(unsigned v0, unsigned v1, unsigned v2);  // per build, see hooks.cpp
void LoadLearnedSites();           // shadow / chat-body call sites learned on this build before
bool HookClientText();            // CFonts::DrawText / DrawLittleText (0.3.DL)
bool ClientTextWanted(const char* text, int len);   // does this line need this mod's font?
void UnhookClientText();
void DrawClientTextNow(std::vector<DrawItem>& items);   // paints them immediately
void RetryHookFontVtable();                            // every ID3DXFont, once it exists
void RetryHookWrapperVtable();                         // the client CFontWrapper vtable
void LogFontTextSummary();                              // per-second take/drop/pass counts
bool MenuWindowActive();   // a native menu is on screen (see hooks.cpp)
bool ScoreboardUp();       // the SA-MP scoreboard panel is open (see hooks.cpp)
void UnhookAll();




// ---------------------------------------------------------------------------
// config.cpp - configuration file schema
//
//   [font]
//   file =                 font file (relative to the game dir or absolute)
//   family =               family name; empty = read it from the file
//   collection = 0         index inside a .ttc
//   size_divisor = 50      pixel height = letter_height * screen_h / size_divisor
//   baseline_offset = 0    global vertical nudge in pixels
//   shadow = on            draw a shadow copy
//   shadow_offset = 1, 1   shadow dx, dy
//
//   letter_width = on      use the client's per-glyph advance
//   client_glyphs = on     draw the symbol the client's face draws for a few
//                          punctuation characters (face 2: ']' is a star)
//
//   [textdraw]
//   fonts = 0, 1, 2, 3     which TextDrawFont faces are taken over
//   brace_colors = off     also read {RRGGBB} braces as colour changes
//   scoreboard_layer = under   under | hide; under draws the GameText layer
//                          before the panel so the panel covers it
//
//   [menu]
//   enabled = on           take over the native menu text
//
//   [overlay]
//   hook_point = endscene  present | endscene; endscene (default) lets other overlays
//                          (omp-client's F9 window, RTSS, ImGui mods) cover us
//
//   [gametext]
//   enabled = on
//   ignore_styles = none   styles kept on the game's own renderer (0..6)
//   scale = 0.82           multiplier for the built-in per-style text height
//
//   [encoding]
//   charset = auto         auto | gbk | big5 | sjis | euckr | cp1251 | utf-8 | <number>
//   utf8_first = on        try strict UTF-8 before the charset above
//   detect = on            let compact_enc_det guess when charset = auto
//
//   [debug]
//   verbose = off          per-texture / per-quad logging
//
// Value syntax: booleans take on/off, true/false, yes/no, 1/0.
// ---------------------------------------------------------------------------
#include "common.h"
#include <cstdio>
#include <map>
#include <algorithm>

Config       g_cfg;
std::wstring g_gameDir;
std::wstring g_pluginStem;
std::wstring g_fontFaces[kFontFaceCount];
HMODULE      g_sampBase = nullptr;

UINT         g_codePage = 0;        // 0 = auto (GetACP())
bool         g_utf8First = true;
bool         g_detectCharset = true;
int          g_singleBytePolicy = 1;   // [encoding] single_byte: 0 system, 1 detect, >1 forced

static std::map<std::wstring, std::wstring> g_ini;

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------
static std::wstring Trim(const std::wstring& s)
{
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return std::wstring();
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)::towlower(c);
    return s;
}

// "on" / "off" / "true" / "false" / "yes" / "no" / "1" / "0"
static bool ParseBool(const std::wstring& v, bool def)
{
    std::wstring s = Lower(Trim(v));
    if (s.empty()) return def;
    if (s == L"on" || s == L"true" || s == L"yes" || s == L"1")  return true;
    if (s == L"off" || s == L"false" || s == L"no" || s == L"0") return false;
    return def;
}

// "1, 1" -> 1, 1
static bool ParsePair(const std::wstring& v, float* a, float* b)
{
    size_t comma = v.find(L',');
    if (comma == std::wstring::npos) return false;
    wchar_t* end = nullptr;
    std::wstring left = Trim(v.substr(0, comma));
    std::wstring right = Trim(v.substr(comma + 1));
    double x = ::wcstod(left.c_str(), &end);
    if (end == left.c_str()) return false;
    double y = ::wcstod(right.c_str(), &end);
    if (end == right.c_str()) return false;
    *a = (float)x;
    *b = (float)y;
    return true;
}

// Comma or space separated id list -> bit mask. `empty` says what an empty
// value means (0 or all bits); the literal "all" always means all bits and
// "none" always means zero.
static DWORD ParseIdList(const std::wstring& value, const wchar_t* key, DWORD empty)
{
    std::wstring s = Trim(value);
    if (s.empty()) return empty;
    std::wstring low = Lower(s);
    if (low == L"all")  return 0xFFFFFFFFu;
    if (low == L"none") return 0;

    DWORD mask = 0;
    std::wstring cur;
    s += L',';
    for (wchar_t ch : s) {
        if (ch == L',' || ch == L' ' || ch == L'\t' || ch == L';') {
            if (!cur.empty()) {
                wchar_t* end = nullptr;
                long v = ::wcstol(cur.c_str(), &end, 10);
                if (end != cur.c_str() && v >= 0 && v < 32) mask |= (1u << v);
                else LogW(L"%s: '%s' is not an id, ignored", key, cur.c_str());
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    return mask;
}
// Charset names (and raw code page numbers) for the "charset" key
static UINT ParseCharset(const std::wstring& v, bool* ok)
{
    *ok = true;
    std::wstring s = Lower(Trim(v));
    if (s.empty() || s == L"auto") return 0;

    if (s == L"utf-8" || s == L"utf8")                 return CP_UTF8;
    if (s == L"gbk" || s == L"gb2312" || s == L"gb18030") {
        if (s == L"gb18030" && ::IsValidCodePage(54936)) return 54936;
        return 936;
    }
    if (s == L"big5")                                  return 950;
    if (s == L"sjis" || s == L"shift-jis" || s == L"shift_jis" || s == L"cp932") return 932;
    if (s == L"euc-jp" || s == L"eucjp")               return ::IsValidCodePage(51932) ? 51932 : 932;
    if (s == L"euc-kr" || s == L"euckr" || s == L"korean") return 949;
    if (s == L"cp1251" || s == L"cyrillic")            return 1251;
    if (s == L"cp1252" || s == L"latin1" || s == L"western") return 1252;
    if (s == L"cp1250")                                return 1250;
    if (s == L"cp1253" || s == L"greek")               return 1253;
    if (s == L"cp1254" || s == L"turkish")             return 1254;
    if (s == L"cp1255" || s == L"hebrew")              return 1255;
    if (s == L"cp1256" || s == L"arabic")              return 1256;
    if (s == L"cp1257" || s == L"baltic")              return 1257;
    if (s == L"cp1258" || s == L"vietnamese")          return 1258;
    if (s == L"koi8-r" || s == L"koi8r")               return 20866;
    if (s == L"koi8-u" || s == L"koi8u")               return 21866;
    if (s == L"cp866")                                 return 866;
    if (s == L"thai" || s == L"tis-620")               return 874;
    if (s == L"iso-8859-1")                            return 28591;
    if (s == L"iso-8859-2")                            return 28592;

    wchar_t* end = nullptr;
    long n = ::wcstol(s.c_str(), &end, 10);
    if (end != s.c_str() && n > 0 && n < 65536 && ::IsValidCodePage((UINT)n))
        return (UINT)n;

    *ok = false;
    return 0;
}

// ---------------------------------------------------------------------------
// File decoding: BOM detection, then UTF-8 -> system ANSI fallback
// ---------------------------------------------------------------------------
static std::wstring DecodeText(const std::vector<BYTE>& raw)
{
    // An odd length means the file was truncated in the middle of a UTF-16 unit
    // (hand-edited, or copied with the wrong byte count). Decoding the even
    // prefix beats losing the last byte silently inside wstring's constructor.
    if ((raw.size() & 1) && raw.size() >= 2 &&
        ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF) ||
         (raw.size() >= 8 && raw[1] == 0 && raw[3] == 0))) {
        LogW(L"config file has an odd byte count (%d), ignoring the last byte", (int)raw.size());
    }

    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        return std::wstring((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
    }
    if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        size_t n = (raw.size() - 2) / 2;
        std::wstring out(n, L'\0');
        for (size_t i = 0; i < n; i++)
            out[i] = (wchar_t)((raw[2 + i * 2] << 8) | raw[3 + i * 2]);
        return out;
    }
    // BOM-less UTF-16LE heuristic: every other byte is zero
    if (raw.size() >= 8) {
        size_t zeros = 0, total = 0;
        for (size_t i = 1; i < raw.size() && total < 128; i += 2, total++)
            if (raw[i] == 0) zeros++;
        if (total >= 8 && zeros * 10 >= total * 9)
            return std::wstring((const wchar_t*)raw.data(), raw.size() / 2);
    }
    size_t off = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
    return ToWide((const char*)raw.data() + off, (int)(raw.size() - off));
}

static bool ReadWholeFile(const std::wstring& path, std::vector<BYTE>& out)
{
    FILE* fp = nullptr;
    ::_wfopen_s(&fp, path.c_str(), L"rb");
    if (!fp) return false;
    ::fseek(fp, 0, SEEK_END);
    long n = ::ftell(fp);
    ::fseek(fp, 0, SEEK_SET);
    if (n > 0) {
        out.resize((size_t)n);
        if (::fread(out.data(), 1, (size_t)n, fp) != (size_t)n) { ::fclose(fp); return false; }
    }
    ::fclose(fp);
    return true;
}

// ---------------------------------------------------------------------------
// Default template
// ---------------------------------------------------------------------------
static void WriteDefaultIni(const std::wstring& path)
{
    static const wchar_t* kTemplate =
        L"; samp-unicode.ini\r\n"
        L"; Re-renders SA-MP TextDraw / GameText with a real Unicode font.\r\n"
        L"; File encoding does not matter here: UTF-8, UTF-16 and ANSI are all accepted.\r\n"
        L"; Booleans accept on/off, true/false, yes/no, 1/0.\r\n"
        L"\r\n"
        L"[font]\r\n"
        L"; ----------------------------------------------------------------\r\n"
        L"; How the two font layers fit together\r\n"
        L"; ----------------------------------------------------------------\r\n"
        L"; Every character is drawn with the face of its TextDrawFont id, i.e.\r\n"
        L"; font0..font3 below. If that face has no glyph for the character -\r\n"
        L"; all Chinese/Japanese/Korean in these four Latin faces - the display\r\n"
        L"; font from section 1 draws it instead. So: Latin/digits/symbols come\r\n"
        L"; from font0..3, CJK always comes from file/family.\r\n"
        L"; ----------------------------------------------------------------\r\n"
        L"; 1) Display font - the CJK fallback, must carry the station charset\r\n"
        L"; ----------------------------------------------------------------\r\n"
        L"; A font file (.ttf/.otf/.ttc). A relative path is looked up next to\r\n"
        L"; this plugin first (modloader\\samp-unicode\\), then in the game folder\r\n"
        L"; (next to gta_sa.exe); an absolute path is used as is. Leave empty to\r\n"
        L"; use the first font file found next to the plugin, then a system font.\r\n"
        L"file =\r\n"
        L"; Family name of the display font. Leave empty to read it from the file\r\n"
        L"; (recommended).\r\n"
        L"family =\r\n"
        L"; Index inside a .ttc collection (a .ttc holds several faces, e.g. the\r\n"
        L"; SC/TC/JP/KR cuts of one CJK family).\r\n"
        L"collection = 0\r\n"
        L"; Family name for a .ttc whose name table is unusable (simsun.ttc has an\r\n"
        L"; empty one on its first face). Leave empty to read it from the file, and\r\n"
        L"; set it only when the log says the family name could not be read.\r\n"
        L"face =\r\n"
        L"; ----------------------------------------------------------------\r\n"
        L"; 2) The four TextDrawFont faces (Latin letters, digits, symbols)\r\n"
        L"; ----------------------------------------------------------------\r\n"
        L"; The original renderer has 0 = blackletter, 1 = bold sans,\r\n"
        L"; 2 = wide capitals (Bank Gothic), 3 = Pricedown. GameText uses them too:\r\n"
        L";   style 0,1,6 -> 3   style 2 -> 0   style 3,4,5 -> 2\r\n"
        L";\r\n"
        L"; Each value is one of\r\n"
        L";   font1 = Arial                  an installed family name\r\n"
        L";   family accepts a list like CSS: entries are tried in order, quotes and\r\n"
        L";   the generic names sans-serif / system-ui / serif are understood.\r\n"
        L";   e.g. family = \"Noto Sans SC\", \"Microsoft YaHei\", sans-serif\r\n"
        L";   font2 = C:\\Fonts\\BankGothic.ttf a font file (no install needed)\r\n"
        L";   font0 =                        empty: use font0.ttf (or\r\n"
        L";                                   samp-unicode-font0.ttf, .otf/.ttc)\r\n"
        L";                                   from this mod folder if present,\r\n"
        L";                                   else the closest installed face\r\n"
        L"; Characters a face does not have (all CJK) are drawn with the display\r\n"
        L"; font above, so \"SAMP 你好\" mixes both without any extra setting.\r\n"
        L"font0 =\r\n"
        L"font1 =\r\n"
        L"font2 =\r\n"
        L"font3 =\r\n"
        L"; ----------------------------------------------------------------\r\n"
        L"; Sizing\r\n"
        L"; ----------------------------------------------------------------\r\n"
        L"; Text pixel height = letter_height * screen_height / size_divisor\r\n"
        L"; bigger divisor = smaller text (60 suits most servers; try 50 to enlarge).\r\n"
        L"size_divisor = 50\r\n"
        L"; Global vertical nudge in pixels; negative moves the text up.\r\n"
        L"baseline_offset = 0\r\n"
        L"; Coverage gamma for every glyph this mod draws. GDI spreads a glyph edge\r\n"
        L"; over more pixels than the client's own rasteriser does, which reads as\r\n"
        L"; soft text beside it; below 1 the middle coverage rises so the stroke keeps\r\n"
        L"; its weight and the edge tightens. 1.00 = GDI's coverage untouched.\r\n"
        L"glyph_gamma = 1.00\r\n"
        L"; Fill each glyph from its outline, the way the client's own D3DXFont does.\r\n"
        L"; GetGlyphOutline is unavailable on some systems (every format returns 1003),\r\n"
        L"; so off is the default; on falls back to GDI by itself when it fails.\r\n"
        L"outline_raster = off\r\n"
        L"; Gray edges instead of hard ones. The client creates its own font with\r\n"
        L"; ANTIALIASED_QUALITY, so on is the faithful setting; off gives the hard 1-bit\r\n"
        L"; stencil. on is what this mod ships with.\r\n"
        L"antialias = on\r\n"

        L"; Draw a shadow copy behind the text.\r\n"
        L"shadow = on\r\n"
        L"; Keep the TextDrawLetterSize aspect: stretch the text horizontally to the\r\n"
        L"; width the game reserved (letterWidth * screen width / 640 per byte).\r\n"
        L"letter_width = on\r\n"
        L"; on = draw the symbol the client draws for a few punctuation characters\r\n"
        L"; (face 2 shows a star for ']' and nothing for '['), off = plain brackets\r\n"
        L"client_glyphs = on\r\n"

        L"\r\n[paths]\r\n"
        L"; The text the client itself draws with ID3DXFont: chat lines, all dialog\r\n"
        L"; styles, the TAB scoreboard, 3D labels, nametags, chat bubbles, the death\r\n"
        L"; window, object material text and the license plate. The client decodes\r\n"
        L"; those bytes with the system ANSI code page, so anything outside it shows\r\n"
        L"; as mojibake; this layer decodes them itself and draws them with the face\r\n"
        L"; below. off = leave the client alone, log = report only.\r\n"
        L"client_text = draw\r\n"
        L"; Face for that text: 0..3 as in [font], or 4 = the display face. 4 is the\r\n"
        L"; default so every surface draws one consistent face, and CJK needs no\r\n"
        L"; fallback at all; 1 (Arial) matches the client's own Latin glyphs exactly.\r\n"
        L"client_text_font = 4\r\n"
        L"; The chat has its own face: the client draws it with Arial and the chat is\r\n"
        L"; where most CJK text arrives, so it defaults to the display face too.\r\n"
        L"client_text_chat_font = 4\r\n"
        L"; the client does not squeeze, so 1.00 is the natural width).\r\n"
        L"; 0 = follow the client's own font weight, else 100..900.\r\n"
        L"client_text_weight = 0\r\n"
        L"; How many copies of the line its shadow has. 2 = exactly what the client\r\n"
        L"; draws (4 copies in a cross up to 1280 px wide, 8 above), 1 = the four-copy\r\n"
        L"; cross only, 0 = no shadow. F7 still switches what the client itself does.\r\n"
        L"client_text_chat_shadow = 2\r\n"
        L"; Alpha of those copies (255 = the opaque black the client uses).\r\n"
        L"client_text_shadow_alpha = 255\r\n"
        L"; Drop the client's own shadow passes where this mod draws the line. On is\r\n"
        L"; what makes the line appear once; off leaves the client's copies beside ours.\r\n"
        L"client_text_drop_shadows = on\r\n"
        L"; Take the client's text at the font level and at the wrapper vtable. Both\r\n"
        L"; are found structurally and learn the call sites they need at runtime.\r\n"
        L"client_text_font_level = on\r\n"
        L"client_text_wrapper = on\r\n"
        L"; Also take over the wide entry point: dialog titles, DXUT buttons (the\r\n"
        L"; spawn screen's << >> and Spawn) and the list captions. Off puts them\r\n"
        L"; back on the client's own bitmap font, which is what used to happen.\r\n"
        L"client_text_wrapper_wide = on\r\n"
        L"; Draw pure-ASCII client text too. 'ours' takes it, 'client' leaves plain\r\n"
        L"; ASCII to the client's own font (an all-English line then looks unmodded).\r\n"
        L"client_text_ascii = ours\r\n"
        L"; Parse {RRGGBB} in chat and dialogs (the client's own chat syntax).\r\n"
        L"client_text_braces = on\r\n"
        L"; Spaces a tab expands to in the dialog styles that do not lay cells out.\r\n"
        L"client_text_tab = 4\r\n"
        L"; Line spacing multiplier for multi-line client text.\r\n"
        L"client_text_line = 1.05\r\n"
        L"; auto = follow the client (recommended), on/off forces a shadow on text the\r\n"
        L"; client draws without one (dialog bodies, captions).\r\n"
        L"client_text_outline = auto\r\n"

        L"\r\n[overlay]\r\n"
        L"; endscene = draw at EndScene, so other overlays (omp-client's settings\r\n"
        L"; window, RTSS, other ImGui mods) are drawn above us (default)\r\n"
        L"; present = draw at Present, i.e. above every other overlay - that covered\r\n"
        L"; omp-client's own window, which is why it is no longer the default\r\n"
        L"hook_point = endscene\r\n"
        L"; Shadow offset in pixels: dx, dy\r\n"
        L"shadow_offset = 1, 1\r\n"
        L"\r\n"
        L"[textdraw]\r\n"
        L"; Which TextDraw font ids to re-render. 0..3 are real text fonts,\r\n"
        L"; 4 = sprite and 5 = 3D model preview are never touched.\r\n"
        L"; Empty or 'all' means every font id.\r\n"
        L"fonts = 0, 1, 2, 3\r\n"
        L"; How the game's own text is hidden: the string is kept and only its colours\r\n"
        L"; are zeroed, so the box and the client's own hit rectangle keep the real\r\n"
        L"; metrics. There is no second mode: replacing the string would move them.\r\n"
        L"; {RRGGBB} colour codes inside a string are NOT part of the client's\r\n"
        L"; TextDraw/GameText syntax (open.mp: \"does not work in textdraws\"), so by\r\n"
        L"; default they are drawn as written. on = this mod parses them, and also\r\n"
        L"; accepts the eight-digit {RRGGBBAA} form. An extension, not parity.\r\n"
        L"brace_colors = off\r\n"
        L"; under = the GameText layer is drawn before the panel and shows through\r\n"
        L"; it (the panel is translucent, like the client's own GameText); hide = skip\r\n"
        L"scoreboard_layer = under\r\n"

        L"\r\n"
        L"[menu]\r\n"
        L"; Native menus (GTA CMenu). The game draws the menu text with its bitmap\r\n"
        L"; font, so non-ASCII arrives as mojibake. on = the plugin replaces those\r\n"
        L"; strings with spaces and draws them itself, at the position and size the\r\n"
        L"; game used. Chat, dialogs and the scoreboard are drawn by the [paths]\r\n"
        L"; layer instead; this section is the game's own menus only.\r\n"
        L"enabled = on\r\n"
        L"; Which of the four faces draws the menu rows (0..3, see [font]).\r\n"
        L"font = 2\r\n"
        L"; The game draws the title with a heavier face than the rows.\r\n"
        L"title_font = 0\r\n"
        L"; Extra size and vertical offset for the title only. 1.0 / 0 draws it at\r\n"
        L"; exactly the size and baseline the game asked for; raise the scale or the\r\n"
        L"; offset if the title should be bigger or sit lower than that.\r\n"
        L"title_scale = 1.00\r\n"
        L"title_dy = 0\r\n"
        L"; Glyph width of the menu title relative to a row glyph. The title face is\r\n"
        L"; tall and narrow, so the title is drawn squeezed; 0.80 reproduces the width\r\n"
        L"; the client gives it. Lower = narrower letters, higher = wider.\r\n"
        L"title_width = 0.80\r\n"
        L"; Horizontal squeeze for menu text. GDI advances are a little wider than the\r\n"
        L"; client's own font metrics, so 0.95..1.00 brings the line width closer.\r\n"
        L"hscale = 1.00\r\n"
        L"; Alignment of the original pen: 0 = text starts at x, 1 = centred, 2 = right.\r\n"
        L"align = 0\r\n"
        L"; Size multiplier for menu text (1.0 = the size the game itself uses).\r\n"
        L"glyph = 1.0\r\n"
        L"; Black outline around menu text in pixels; the game draws one, so this\r\n"
        L"; keeps our letters looking like the ones the game still draws (pure ASCII\r\n"
        L"; menu rows are left to the game, which is why they keep their outline).\r\n"
        L"outline = 1\r\n"
        L"; Extra drop shadow, read at the same 20-px reference as the outline and\r\n"
        L"; drawn underneath it, so it has to be larger than the outline to show.\r\n"
        L"shadow = 3\r\n"
        L"; The title is drawn much larger than the rows, so it carries its own pair\r\n"
        L"; (both are read at the same 20-px reference size / in pixels).\r\n"
        L"title_outline = 2\r\n"
        L"title_shadow = 0\r\n"
        L"; Drop shadow colour and opacity. 000000 with 255 is the opaque black the\r\n"
        L"; game itself uses; lower the alpha for a softer, grey shadow.\r\n"
        L"shadow_color = 000000\r\n"
        L"shadow_alpha = 255\r\n"
        L"; Colour as RRGGBB, or 0 = use the colour the game set for the line.\r\n"
        L"color = 0\r\n"
        L"\r\n"
        L"[gametext]\r\n"
        L"; Re-render server GameTextForPlayer / GameTextForAll announcements.\r\n"
        L"enabled = on\r\n"
        L"; Fade the last 400 ms. On by default: the background is restored\r\n"
        L"; first, so the fade blends over clean pixels and leaves nothing behind.\r\n"
        L"fade_out = on\r\n"
        L"; Styles that must NOT be re-rendered (comma separated, 0..6).\r\n"
        L"; 'none' (the default) re-renders every style, 'all' disables it.\r\n"
        L"ignore_styles = none\r\n"
        L"; Multiplier for the built-in per-style text height (1.0 = native size).\r\n"
        L"scale = 0.82\r\n"
        L"\r\n"
        L"[encoding]\r\n"
        L"; Charset of the text sent by the server.\r\n"
        L";   auto    follow the system code page (936 GBK, 950 Big5, 932 Shift-JIS,\r\n"
        L";           949 EUC-KR, 1251 Cyrillic, 1252 Western Europe, ...)\r\n"
        L";   names   gbk, gb18030, big5, sjis, euc-kr, cp1251, cp1252, utf-8, ...\r\n"
        L";   numbers 936, 950, 932, 949, 1251, 1252, 65001, ...\r\n"
        L"charset = auto\r\n"
        L"; Try strict UTF-8 first, then fall back to the charset above.\r\n"
        L"utf8_first = on\r\n"
        L"; Draw pure-ASCII text with our renderer instead of leaving it to the client.\r\n"
        L"; On is the default: one face and one size for every string, so a line that\r\n"
        L"; mixes Latin and CJK does not change shape halfway through.\r\n"
        L"redraw_ascii = on\r\n"
        L"; With charset = auto, let compact_enc_det guess the encoding. Only\r\n"
        L"; structurally distinctive results (CJK / Cyrillic) are accepted, so\r\n"
        L"; Latin1/ASCII never override the system default.\r\n"
        L"detect = on\r\n"
        L"; A server that sends Spanish, Polish or Arabic bytes sends the ANSI code page"
        L" of\r\n"
        L"; the machine it was built on: 1252, 1250, 1256. Every high byte is a"
        L" letter\r\n"
        L"; there, so nothing in the stream identifies it. system = keep the Windows"
        L" code\r\n"
        L"; page, detect = use compact_enc_det's single-byte guess, or a number to"
        L" force it.\r\n"
        L"single_byte = detect\r\n"
        L"\r\n"
        L"[debug]\r\n"
        L"; Master switch for the log file: off = no samp-unicode.log at all.\r\n"
        L"log = on\r\n"
        L"; Log every texture build and every quad draw (troubleshooting only;\r\n"
        L"; it writes to disk a lot and can slow the game down).\r\n"
        L"verbose = off\r\n";

    FILE* fp = nullptr;
    ::_wfopen_s(&fp, path.c_str(), L"wb");
    if (!fp) return;

    int n = ::WideCharToMultiByte(CP_UTF8, 0, kTemplate, -1, nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        std::vector<char> buf((size_t)n);
        ::WideCharToMultiByte(CP_UTF8, 0, kTemplate, -1, buf.data(), n, nullptr, nullptr);
        static const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
        ::fwrite(kBom, 1, 3, fp);
        ::fwrite(buf.data(), 1, (size_t)(n - 1), fp);
    }
    ::fclose(fp);
}

// ---------------------------------------------------------------------------
static void ParseIni(const std::wstring& text)
{
    g_ini.clear();
    std::wstring section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;

        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            size_t rb = line.find(L']');
            if (rb != std::wstring::npos) section = Lower(Trim(line.substr(1, rb - 1)));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(line.substr(0, eq)));
        std::wstring val = Trim(line.substr(eq + 1));

        // strip a trailing comment only when it is preceded by whitespace
        size_t sc = val.find(L';');
        if (sc != std::wstring::npos) {
            bool spaceBefore = (sc > 0) && (val[sc - 1] == L' ' || val[sc - 1] == L'\t');
            if (spaceBefore) val = Trim(val.substr(0, sc));
        }
        if (!section.empty()) key = section + L"." + key;
        g_ini[key] = val;
    }
}

// "none", "all" or a comma separated list of the bits set.
static std::wstring DescribeMask(DWORD mask)
{
    if (mask == 0xFFFFFFFFu) return L"all";
    if (mask == 0) return L"none";

    std::wstring out;
    wchar_t one[16];
    for (int i = 0; i < 32; i++) {
        if (!(mask & (1u << i))) continue;
        swprintf_s(one, L"%s%d", out.empty() ? L"" : L",", i);
        out += one;
    }
    return out;
}


void LoadConfig()
{
    std::wstring primary = g_gameDir + g_pluginStem + L".ini";

    std::vector<BYTE> raw;
    if (!ReadWholeFile(primary, raw)) {
        LogW(L"no config file found, using defaults: %s", primary.c_str());
        WriteDefaultIni(primary);
        LogW(L"default config written: %s", primary.c_str());
    }
    if (!raw.empty()) ParseIni(DecodeText(raw));

    // ---- [font] ----
    g_cfg.file            = IniGetStr(L"font.file", L"");
    g_cfg.family          = IniGetStr(L"font.family", L"");
    g_cfg.collectionIndex = IniGetInt(L"font.collection", 0);
    g_cfg.fontFace = IniGetStr(L"font.face", L"");
    for (int i = 0; i < kFontRoleCount; i++) {
        wchar_t key[16];
        swprintf_s(key, L"font.font%d", i);
        g_cfg.face[i] = IniGetStr(key, L"");
    }
    g_cfg.sizeDivisor     = IniGetFloat(L"font.size_divisor", 50.0f);
    g_cfg.baselineOffset  = IniGetFloat(L"font.baseline_offset", 0.0f);
    g_cfg.shadow          = ParseBool(IniGetStr(L"font.shadow", L"on"), true);
    // How the glyph coverage GDI produces is mapped before it becomes the glyph's alpha.
    // Below one the middle coverage is pulled up: the stroke keeps its weight and the
    // edge gets tighter, which is what makes the text read as crisp as the client's own
    // rasteriser instead of soft. 1.0 leaves GDI's coverage untouched.
    g_cfg.glyphGamma = IniGetFloat(L"font.glyph_gamma", 1.00f);
    if (g_cfg.glyphGamma < 0.30f) g_cfg.glyphGamma = 0.30f;
    if (g_cfg.glyphGamma > 2.00f) g_cfg.glyphGamma = 2.00f;
    g_cfg.fontAntialias = ParseBool(IniGetStr(L"font.antialias", L"off"), false);
    g_cfg.outlineRaster = ParseBool(IniGetStr(L"font.outline_raster", L"off"), false);
    g_cfg.respectLetterWidth = ParseBool(IniGetStr(L"font.letter_width", L"on"), true);
    // The client's faces draw a star for ']' in face 2 (and nothing for '['); on by
    // default so a line looks the way the client draws it, off for plain brackets.
    g_cfg.clientGlyphs = ParseBool(IniGetStr(L"font.client_glyphs", L"on"), true);

    // [paths] client_text: what to do with the text the client's own UI draws
    // (scoreboard, dialogs, labels, nametags, bubbles, chat): off leaves it alone,
    // log only reports it, draw replaces it with this font.
    {
        std::wstring t = IniGetStr(L"paths.client_text", L"draw");
        for (size_t i = 0; i < t.size(); i++) t[i] = towlower(t[i]);
        g_cfg.clientText = (t == L"off") ? 0 : (t == L"log") ? 1 : 2;

        g_cfg.clientTextFont = IniGetInt(L"paths.client_text_font", 4);
        if (g_cfg.clientTextFont < 0) g_cfg.clientTextFont = 0;
        if (g_cfg.clientTextFont > kFontDefault) g_cfg.clientTextFont = kFontDefault;

        g_cfg.clientTextFontLevel = ParseBool(IniGetStr(L"paths.client_text_font_level", L"on"), true);
        g_cfg.clientTextWrapper = ParseBool(IniGetStr(L"paths.client_text_wrapper", L"on"), true);
        g_cfg.clientTextWrapperWide =
            ParseBool(IniGetStr(L"paths.client_text_wrapper_wide", L"on"), true);
        g_cfg.clientTextChatFont = IniGetInt(L"paths.client_text_chat_font", 4);
        if (g_cfg.clientTextChatFont < 0) g_cfg.clientTextChatFont = 0;
        if (g_cfg.clientTextChatFont > kFontDefault) g_cfg.clientTextChatFont = kFontDefault;

        // The client's chat shadow is not a ring: four copies in a cross up to 1280 px
        // wide, and eight of them - diagonals at one pixel, straight offsets at two -
        // above that. 2 reproduces it, 1 forces the four-copy cross, 0 drops it.
        g_cfg.clientTextChatShadow = IniGetInt(L"paths.client_text_chat_shadow", 2);
        if (g_cfg.clientTextChatShadow < 0) g_cfg.clientTextChatShadow = 0;
        if (g_cfg.clientTextChatShadow > 2) g_cfg.clientTextChatShadow = 2;

        {
            std::wstring a = IniGetStr(L"paths.client_text_ascii", L"ours");
            for (size_t i = 0; i < a.size(); i++) a[i] = towlower(a[i]);
            g_cfg.clientTextAsciiOurs = (a == L"ours");
        }

        g_cfg.clientTextWeight = IniGetInt(L"paths.client_text_weight", 400);
        if (g_cfg.clientTextWeight < 100 || g_cfg.clientTextWeight > 900)
            g_cfg.clientTextWeight = kClientTextWeightDefault;

        g_cfg.clientTextDropShadows = ParseBool(IniGetStr(L"paths.client_text_drop_shadows", L"off"), false);
        g_cfg.clientTextShadowAlpha = IniGetInt(L"paths.client_text_shadow_alpha", 0);
        if (g_cfg.clientTextShadowAlpha < 0)   g_cfg.clientTextShadowAlpha = 0;
        if (g_cfg.clientTextShadowAlpha > 255) g_cfg.clientTextShadowAlpha = 255;

        g_cfg.clientTextBraces = ParseBool(IniGetStr(L"paths.client_text_braces", L"on"), true);

        g_cfg.clientTextTab = IniGetInt(L"paths.client_text_tab", 4);
        if (g_cfg.clientTextTab < 1) g_cfg.clientTextTab = 1;
        if (g_cfg.clientTextTab > 16) g_cfg.clientTextTab = 16;

        g_cfg.clientTextLine = IniGetFloat(L"paths.client_text_line", 1.05f);
        if (g_cfg.clientTextLine < 0.8f) g_cfg.clientTextLine = 0.8f;
        if (g_cfg.clientTextLine > 2.0f) g_cfg.clientTextLine = 2.0f;

        std::wstring ol = IniGetStr(L"paths.client_text_outline", L"auto");
        for (size_t i = 0; i < ol.size(); i++) ol[i] = towlower(ol[i]);
        g_cfg.clientTextOutline = (ol == L"on") ? 1 : (ol == L"off") ? 0 : -1;
    }

    // [overlay] hook_point: endscene (default; omp-client draws its own window at Present, so
    // drawing there put this overlay on top of it) or present (above every other overlay).
    {
        const std::wstring where = IniGetStr(L"overlay.hook_point", L"endscene");
        g_cfg.hookEndScene = (::_wcsicmp(where.c_str(), L"endscene") == 0);
    }
    {
        float dx = 1.0f, dy = 1.0f;
        if (ParsePair(IniGetStr(L"font.shadow_offset", L"1, 1"), &dx, &dy)) {
            g_cfg.shadowOffsetX = (int)dx;
            g_cfg.shadowOffsetY = (int)dy;
        }
    }
    if (g_cfg.sizeDivisor < 1.0f) g_cfg.sizeDivisor = 1.0f;

    // ---- [textdraw] ----
    g_cfg.fontMask = ParseIdList(IniGetStr(L"textdraw.fonts", L"0, 1, 2, 3"),
                                 L"textdraw.fonts", 0xFu);

    g_cfg.braceColors = ParseBool(IniGetStr(L"textdraw.brace_colors", L"off"), false);
    {
        const std::wstring layer = IniGetStr(L"textdraw.scoreboard_layer", L"under");
        g_cfg.scoreboardUnder = (::_wcsicmp(layer.c_str(), L"hide") != 0) &&
                                (::_wcsicmp(layer.c_str(), L"off") != 0);
    }

    // ---- [menu] ----
    g_cfg.menuEnabled = ParseBool(IniGetStr(L"menu.enabled", L"on"), true);
    g_cfg.menuFont      = (int)IniGetInt(L"menu.font", 2);
    g_cfg.menuTitleFont = (int)IniGetInt(L"menu.title_font", 0);
    g_cfg.menuTitleScale = (float)IniGetFloat(L"menu.title_scale", 1.0);
    g_cfg.menuTitleDy    = (float)IniGetFloat(L"menu.title_dy", 0.0);
    g_cfg.menuHScale     = (float)IniGetFloat(L"menu.hscale", 1.0);
    g_cfg.menuTitleWidth = IniGetFloat(L"menu.title_width", 0.80f);
    g_cfg.menuAlign   = (int)IniGetInt(L"menu.align", 0);
    g_cfg.menuGlyph   = (float)IniGetFloat(L"menu.glyph", 1.0);
    g_cfg.menuOutline   = (int)IniGetInt(L"menu.outline", 1);
    if (g_cfg.menuOutline < 0) g_cfg.menuOutline = 0;   // a negative size would make the
    if (g_cfg.menuOutline > 8) g_cfg.menuOutline = 8;   // vertex reserve explode
    g_cfg.menuShadow    = (int)IniGetInt(L"menu.shadow", 3);
    g_cfg.menuTitleOutline = (int)IniGetInt(L"menu.title_outline", 2);
    g_cfg.menuTitleShadow  = (int)IniGetInt(L"menu.title_shadow", 0);
    if (g_cfg.menuShadow < 0) g_cfg.menuShadow = 0;
    if (g_cfg.menuShadow > 16) g_cfg.menuShadow = 16;
    if (g_cfg.menuTitleOutline < 0) g_cfg.menuTitleOutline = 0;
    if (g_cfg.menuTitleOutline > 8) g_cfg.menuTitleOutline = 8;
    if (g_cfg.menuTitleShadow < 0) g_cfg.menuTitleShadow = 0;
    if (g_cfg.menuTitleShadow > 16) g_cfg.menuTitleShadow = 16;
    g_cfg.menuShadowAlpha = (int)IniGetInt(L"menu.shadow_alpha", 255);
    if (g_cfg.menuTitleWidth < 0.2f) g_cfg.menuTitleWidth = 0.2f;
    if (g_cfg.menuTitleWidth > 2.0f) g_cfg.menuTitleWidth = 2.0f;
    if (g_cfg.menuShadowAlpha < 0) g_cfg.menuShadowAlpha = 0;
    if (g_cfg.menuShadowAlpha > 255) g_cfg.menuShadowAlpha = 255;
    {
        std::wstring c = Trim(IniGetStr(L"menu.color", L"0"));
        g_cfg.menuColor = (DWORD)::wcstoul(c.c_str(), nullptr, 16);
        // Shadow colour shares the RRGGBB syntax of menu.color; its alpha is the
        // separate shadow_alpha, which is what makes it look like a shadow instead
        // of a second, offset outline.
        std::wstring sc = Trim(IniGetStr(L"menu.shadow_color", L"000000"));
        g_cfg.menuShadowColor = (DWORD)::wcstoul(sc.c_str(), nullptr, 16) & 0x00FFFFFFu;
    }

    // ---- [gametext] ----
    g_cfg.gameTextEnabled = ParseBool(IniGetStr(L"gametext.enabled", L"on"), true);
    g_cfg.gameTextIgnore  = ParseIdList(IniGetStr(L"gametext.ignore_styles", L"none"),
                                        L"gametext.ignore_styles", 0);
    // 0.82 is measured against the client's own GameText: its cap height is 45 px
    // where ours was 55 at scale 1.0 on the same style and screen.
    g_cfg.gameTextScale   = IniGetFloat(L"gametext.scale", 0.82f);
    g_cfg.gameTextFadeOut = ParseBool(IniGetStr(L"gametext.fade_out", L"on"), true);
    if (g_cfg.gameTextScale < 0.2f) g_cfg.gameTextScale = 0.2f;
    if (g_cfg.gameTextScale > 4.0f) g_cfg.gameTextScale = 4.0f;

    // ---- [encoding] ----
    {
        bool ok = true;
        UINT cp = ParseCharset(IniGetStr(L"encoding.charset", L"auto"), &ok);
        if (!ok)
            LogW(L"charset value is not recognised, using auto");
        g_cfg.codePage    = (int)cp;
        g_cfg.utf8First   = ParseBool(IniGetStr(L"encoding.utf8_first", L"on"), true);
        g_cfg.detect      = ParseBool(IniGetStr(L"encoding.detect", L"on"), true);
        g_cfg.redrawAscii = ParseBool(IniGetStr(L"encoding.redraw_ascii", L"on"), true);
        // Spanish 1252, Polish 1250 and Arabic 1256 reach the client as the ANSI bytes of the
        // machine the server was built on. Every high byte is a letter there, so nothing in the
        // stream identifies the code page; this key chooses who decides.
        {
            std::wstring sb = IniGetStr(L"encoding.single_byte", L"detect");
            for (size_t i = 0; i < sb.size(); i++) sb[i] = (wchar_t)towlower(sb[i]);
            if (sb == L"system" || sb == L"off" || sb == L"0") {
                g_singleBytePolicy = 0;
            } else if (sb == L"detect" || sb == L"auto" || sb == L"on") {
                g_singleBytePolicy = 1;
            } else {
                bool okSb = true;
                const UINT fcp = ParseCharset(sb, &okSb);
                if (okSb && fcp > 1) {
                    g_singleBytePolicy = (int)fcp;
                } else {
                    g_singleBytePolicy = 1;
                    LogW(L"single_byte value is not recognised, using detect");
                }
            }
        }
        g_codePage        = cp;
        g_utf8First       = g_cfg.utf8First;
        g_detectCharset   = g_cfg.detect;

        // Which decoder this server actually ends up with, in one line: with per-server ini
        // files the answer differs by server, and this is the only place that shows it.
        const std::wstring charsetName = IniGetStr(L"encoding.charset", L"auto");
        const std::wstring singleName  = IniGetStr(L"encoding.single_byte", L"detect");
        wchar_t cpText[16];
        if (cp == 0) swprintf_s(cpText, L"auto");
        else         swprintf_s(cpText, L"%u", cp);
        LogW(L"encoding: charset=%s(%s) utf8_first=%d detect=%d single_byte=%s",
             charsetName.c_str(), cpText, (int)g_cfg.utf8First, (int)g_cfg.detect,
             singleName.c_str());
    }

    // ---- [debug] ----
    g_cfg.logEnabled = ParseBool(IniGetStr(L"debug.log", L"on"), true);
    g_cfg.verbose    = ParseBool(IniGetStr(L"debug.verbose", L"off"), false);

    UINT eff = ResolveCodePage();
    LogW(L"config: file='%s' family='%s' size_divisor=%.1f baseline_offset=%.1f shadow=%d(%d,%d) glyph_gamma=%.2f letter_width=%d",
         g_cfg.file.c_str(), g_cfg.family.c_str(), g_cfg.sizeDivisor, g_cfg.baselineOffset,
         (int)g_cfg.shadow, g_cfg.shadowOffsetX, g_cfg.shadowOffsetY, g_cfg.glyphGamma,
         (int)g_cfg.respectLetterWidth);
    LogW(L"paths: client_text=%s, font=%d, chat_font=%d, braces=%d, tab=%d, line=%.2f, outline=%d",
         g_cfg.clientText == 2 ? L"draw" : g_cfg.clientText == 1 ? L"log" : L"off",
         g_cfg.clientTextFont, g_cfg.clientTextChatFont,
         (int)g_cfg.clientTextBraces, g_cfg.clientTextTab, g_cfg.clientTextLine,
         g_cfg.clientTextOutline);
    LogW(L"charset: %s = %u (%s), utf8_first=%d, detect=%d, redraw_ascii=%d",
         g_cfg.codePage ? L"forced" : L"auto", eff, CodePageName(eff).c_str(),
         (int)g_cfg.utf8First, (int)g_cfg.detect, (int)g_cfg.redrawAscii);
    // A std::wstring, not a fixed buffer: ParseIdList accepts mask 0..31 and the
    // concatenation would overflow a 64-wchar array for every bit set.
    std::wstring fonts = DescribeMask(g_cfg.fontMask);
    LogW(L"overlay: hook_point=%s, client_glyphs=%s",
         g_cfg.hookEndScene ? L"endscene" : L"present",
         g_cfg.clientGlyphs ? L"on" : L"off");
    LogW(L"textdraw: fonts=%s, brace_colors=%s, scoreboard=%s",
         fonts.empty() ? L"(none)" : fonts.c_str(),
         g_cfg.braceColors ? L"on" : L"off",
         g_cfg.scoreboardUnder ? L"under" : L"hide");
    LogW(L"menu: enabled=%s, font=%d, title_font=%d, align=%d, glyph=%.2f",
         g_cfg.menuEnabled ? L"on" : L"off", g_cfg.menuFont, g_cfg.menuTitleFont,
         g_cfg.menuAlign, g_cfg.menuGlyph);
    LogW(L"menu title: scale=%.2f dy=%.0f width=%.2f", g_cfg.menuTitleScale, g_cfg.menuTitleDy,
         g_cfg.menuTitleWidth);

    std::wstring ignored = DescribeMask(g_cfg.gameTextIgnore);

    LogW(L"gametext: enabled=%d, ignore_styles=%s, styles=0..%d (built-in layout), scale=%.2f",
         (int)g_cfg.gameTextEnabled, ignored.c_str(), GameTextStyleCount() - 1, g_cfg.gameTextScale);
}

int IniGetInt(const wchar_t* key, int def)
{
    auto it = g_ini.find(Lower(key));
    if (it == g_ini.end() || it->second.empty()) return def;
    wchar_t* end = nullptr;
    long v = ::wcstol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str()) return def;
    return (int)v;
}

float IniGetFloat(const wchar_t* key, float def)
{
    auto it = g_ini.find(Lower(key));
    if (it == g_ini.end() || it->second.empty()) return def;
    wchar_t* end = nullptr;
    double v = ::wcstod(it->second.c_str(), &end);
    if (end == it->second.c_str()) return def;
    return (float)v;
}

std::wstring IniGetStr(const wchar_t* key, const wchar_t* def)
{
    auto it = g_ini.find(Lower(key));
    if (it == g_ini.end()) return def;
    return it->second;
}

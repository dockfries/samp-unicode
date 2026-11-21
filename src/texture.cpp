// ---------------------------------------------------------------------------
// texture.cpp - GDI rasterization -> 32bpp DIB -> D3D9 texture
//
// Design notes:
//   - size = text extent + 4px padding; max width 4096, max height 512
//     (0x200); anything bigger is skipped entirely
//   - DIB: 32bpp, top-down (biHeight = -h), zero filled first
//   - SetBkMode(TRANSPARENT), SetTextColor(white),
//     DrawTextW(DT_SINGLELINE|DT_NOPREFIX = 0x820)
//   - per pixel: B=G=R=0xFF, A=min(R,G)   (white glyph + coverage as alpha)
//   - CreateTexture(w,h,1,0,D3DFMT_A8R8G8B8(0x15),D3DPOOL_MANAGED(1))
//   - LockRect(0,&lr,0,0) -> per-row memcpy -> UnlockRect(0)
//   - cache key = "<pixel height>|<text>"
// ---------------------------------------------------------------------------
#include "common.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

// Every cache below is touched from two threads: the render thread inside
// EndScene and the main thread (menu strings are measured there). A window resize
// makes it worse, because Reset clears the cache while a frame may be drawing it -
// that was a crash, not a leak.
static CRITICAL_SECTION g_texCs;
static std::once_flag   g_texOnce;

static void TexLockInit() { std::call_once(g_texOnce, [] { ::InitializeCriticalSection(&g_texCs); }); }

void LockTextureCache()   { TexLockInit(); ::EnterCriticalSection(&g_texCs); }
void UnlockTextureCache() { ::LeaveCriticalSection(&g_texCs); }

struct TexGuard { TexGuard() { LockTextureCache(); } ~TexGuard() { UnlockTextureCache(); } };

static std::unordered_map<std::wstring, TexEntry> g_texCache;
static std::unordered_map<std::wstring, int>      g_widthCache;
static size_t            g_texBytes = 0;
static IDirect3DDevice9* g_texDevice = nullptr;

constexpr size_t kTexCacheBudget = 16u * 1024u * 1024u;   // total texture bytes
constexpr int    kMaxTexW        = 4096;                   // one glyph run must fit
constexpr int    kMaxTexH        = 512;                    // inside 4096x512
// One budget per cache, sized for a busy server UI instead of a demo script:
// a long scoreboard or a scrolling help panel used to fill the width cache and
// clear it wholesale, which re-measures every visible string in one frame.
constexpr size_t kWidthCacheEntries = 2048;   // measured run widths
constexpr size_t kLineCacheEntries  = 1024;   // line heights per (face, px)
constexpr size_t kGlyphCacheEntries = 8192;   // can this face draw this char
constexpr size_t kTexCacheEntries   = 512;    // uploaded textures (bytes are capped above)

// ---------------------------------------------------------------------------
static const std::wstring& FaceOf(int fontId)
{
    if (fontId < 0 || fontId >= kFontFaceCount) fontId = kFontDefault;
    return g_fontFaces[fontId];
}

// The weight a glyph is rasterised with. The client asks D3DX for a weight per font and
// GDI draws that weight, so a face at 400 and 700 are visibly different fonts; this mod
// drew everything at 400. Only the client text sets it - the other surfaces keep the
// default, so their textures are byte for byte what they were.
static int g_atlasWeight = FW_NORMAL;
// How many textures have been rasterised since the process started: a frame that rebuilds them
// instead of reusing the cache is the one thing that can cost a second per frame.
static volatile LONG g_textureBuilds = 0;
int TextureBuildCount() { return (int)g_textureBuilds; }

// Defined with the cache; called from GetTextTexture when a new texture no longer fits.
static void EvictOldest();

void SetAtlasWeight(int weight)
{
    g_atlasWeight = (weight > 0 && weight < 1000) ? weight : FW_NORMAL;
}

static HFONT MakeFont(int pxHeight, int fontId)
{
    // No antialiasing by default, because that is what the client's own text is: a scanline
    // through its chat is only ever 255 (glyph), 0 (shadow) or the background - not one
    // intermediate value. Its D3DXCreateFontA(..., quality = 0) resolves to a 1-bit stencil
    // here, and rendering the same text antialiased is what made this mod's chat look hazy
    // beside it. font.antialias = on asks GDI for grayscale instead.

    return ::CreateFontW(pxHeight, 0, 0, 0, g_atlasWeight, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         g_cfg.fontAntialias ? ANTIALIASED_QUALITY
                                              : NONANTIALIASED_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE,
                         FaceOf(fontId).c_str());
}

// GDI is the only rasteriser now: the measurement and the fill come from the same font handle,
// so a texture can never be sized by one and filled by the other.
static SIZE MeasureRun(HDC dc, const std::wstring& text, int pxHeight, int fontId)
{
    (void)pxHeight;
    (void)fontId;
    SIZE sz = { 0, 0 };
    ::GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    return sz;
}

int MeasureTextWidth(const std::wstring& text, int pxHeight, int fontId)
{
    if (text.empty() || pxHeight <= 0) return 0;

    // The measurement caches are static maps touched from the render thread and from
    // the detours, so they take the same lock the texture cache uses.
    TexGuard guard;
    wchar_t prefix[32];
    swprintf_s(prefix, L"%d.%d.%d|", fontId, pxHeight, g_atlasWeight);
    std::wstring key = prefix + text;

    auto cached = g_widthCache.find(key);
    if (cached != g_widthCache.end()) return cached->second;

    HDC dc = ::CreateCompatibleDC(nullptr);
    if (!dc) return 0;
    HFONT fn = MakeFont(pxHeight, fontId);
    HGDIOBJ old = fn ? ::SelectObject(dc, fn) : nullptr;

    const SIZE sz = MeasureRun(dc, text, pxHeight, fontId);
    if (old) ::SelectObject(dc, old);
    if (fn) ::DeleteObject(fn);
    ::DeleteDC(dc);

    if (g_widthCache.size() >= kWidthCacheEntries) {
        LogDbg(L"width cache full (%d entries), cleared", (int)g_widthCache.size());
        g_widthCache.clear();
    }
    g_widthCache[key] = sz.cx;
    return sz.cx;
}

// Coverage as GDI produced it, mapped through the gamma the config asks for. GDI spreads
// a glyph's edge over more pixels than the client's own rasteriser does, so the same
// stroke reads softer; pulling the middle coverage up tightens the edge without touching
// the stroke's weight. A lookup table because this runs per pixel of every new string.
static BYTE CoverageGamma(BYTE cov)
{
    static BYTE  lut[256];
    static float built = -1.0f;
    const float g = (g_cfg.glyphGamma > 0.05f && g_cfg.glyphGamma < 4.0f) ? g_cfg.glyphGamma : 1.0f;
    if (built != g) {
        for (int i = 0; i < 256; i++) {
            const float v = (float)i / 255.0f;
            const float o = (g == 1.0f) ? v : powf(v, g);
            lut[i] = (BYTE)(o * 255.0f + 0.5f);
        }
        built = g;
    }
    return lut[cov];
}

// ---------------------------------------------------------------------------
static LPDIRECT3DTEXTURE9 BuildTexture(IDirect3DDevice9* dev, const std::wstring& text,
                                       int pxHeight, int fontId, int* outW, int* outH,
                                       int* outLogicalW)
{
    if (!dev || text.empty() || pxHeight <= 0) return nullptr;

    HDC dc = ::CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;

    HFONT fn = MakeFont(pxHeight, fontId);
    if (!fn) { ::DeleteDC(dc); return nullptr; }
    HGDIOBJ oldFont = ::SelectObject(dc, fn);

    SIZE sz = MeasureRun(dc, text, pxHeight, fontId);

    // Too long for one texture: cut it down instead of dropping the whole line,
    // so a long message still shows up (visibly shortened) rather than vanishing.
    std::wstring draw = text;
    if (sz.cx > kMaxTexW || sz.cy > kMaxTexH) {
        size_t keep = text.size();
        if (sz.cx > 0) {
            keep = (size_t)((double)text.size() * (double)kMaxTexW / (double)sz.cx);
        }
        if (keep < 1) keep = 1;
        if (keep > text.size()) keep = text.size();
        for (int pass = 0; pass < 6; pass++) {
            draw.assign(text, 0, keep);
            SIZE t = MeasureRun(dc, draw, pxHeight, fontId);
            sz = t;                        // the size of what is actually drawn
            if (t.cx <= kMaxTexW && t.cy <= kMaxTexH) break;
            if (keep <= 1) break;
            keep = keep > 8 ? keep - keep / 8 : keep - 1;
        }
        if (draw != text) {
            LogDbg(L"text truncated: %d -> %d chars at px=%d",
                   (int)text.size(), (int)draw.size(), pxHeight);
        }
    }

    int w = sz.cx + 4;
    int h = sz.cy + 4;
    if (sz.cx <= 0 || sz.cy <= 0 || w > kMaxTexW + 4 || h > kMaxTexH + 4) {
        LogDbg(L"texture skipped: px=%d raw=%dx%d (over the limit)", pxHeight, sz.cx, sz.cy);
        ::SelectObject(dc, oldFont);
        ::DeleteObject(fn);
        ::DeleteDC(dc);
        return nullptr;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        ::SelectObject(dc, oldFont);
        ::DeleteObject(fn);
        ::DeleteDC(dc);
        return nullptr;
    }
    HGDIOBJ oldBmp = ::SelectObject(dc, bmp);
    ::memset(bits, 0, (size_t)w * h * 4);
    bool pixelFilled = false;      // a rasteriser filled 'bits' already; GDI must not run over it

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(255, 255, 255));

    // Two ways to fill the atlas, and they are not equivalent:
    //
    //   DrawTextW       - GDI renders a *hinted* bitmap: every stem snaps to the pixel grid,
    //                     so at chat sizes the letters come out a step bolder and their edges
    //                     spread over more pixels than the client's.
    //   GetGlyphOutline - the glyph *outline*, filled here exactly the way D3DXFont fills it
    //                     (GGO_GRAY8_BITMAP gives 0..64 coverage per pixel, unhinted). This
    //                     is the client's own rasteriser, which is why it is the default.
    //
    // The outline path is per character, so the pen is advanced by each glyph's own cell
    // width; MeasureTextWidth sums the same values when this mode is on, so layout and
    // drawing agree.
    bool outlined = !pixelFilled && g_cfg.outlineRaster;
    if (outlined) {
        // MAT2 holds FIXED values, and FIXED 1.0 is 0x00010000, not 1. A literal 1 here is
        // a matrix that squeezes the glyph to nothing, and GetGlyphOutline then fails - which
        // is what made the outline path fall back to GDI without a word.
        static const MAT2 kIdentity = { { 0x00010000, 0 }, { 0, 0x00010000 } };
        TEXTMETRICW tm = {};
        ::GetTextMetricsW(dc, &tm);
        const int baseline = 2 + (int)tm.tmAscent;
        int penX = 2;
        std::vector<BYTE> buf;
        for (size_t i = 0; i < draw.size() && outlined; i++) {
            GLYPHMETRICS gm = {};
            const DWORD need = ::GetGlyphOutlineW(dc, (UINT)draw[i], GGO_GRAY8_BITMAP, &gm, 0,
                                                  nullptr, &kIdentity);
            if (need == GDI_ERROR) {
                LogDbg(L"outline raster unavailable at px=%d (GetGlyphOutline failed), using GDI", pxHeight);
                outlined = false;
                break;
            }
            if (need > 0) {
                if (buf.size() < need) buf.resize(need);
                if (::GetGlyphOutlineW(dc, (UINT)draw[i], GGO_GRAY8_BITMAP, &gm, need,
                                       buf.data(), &kIdentity) == GDI_ERROR) {
                    outlined = false;
                    break;
                }
                const int gx     = penX + (int)gm.gmptGlyphOrigin.x;
                const int gy     = baseline - (int)gm.gmptGlyphOrigin.y;
                const int stride = (int)((gm.gmBlackBoxX + 3u) & ~3u);
                for (int row = 0; row < (int)gm.gmBlackBoxY; row++) {
                    const int y = gy + row;
                    if (y < 0 || y >= h) continue;
                    const BYTE* src = buf.data() + (size_t)row * (size_t)stride;
                    BYTE* dst = (BYTE*)bits + ((size_t)y * (size_t)w + (size_t)gx) * 4u;
                    for (int col = 0; col < (int)gm.gmBlackBoxX; col++) {
                        const int x = gx + col;
                        if (x < 0 || x >= w) continue;
                        const unsigned v = src[col];
                        if (!v) continue;
                        const BYTE cov = (BYTE)((v * 255u) / 64u);
                        dst[col * 4 + 0] = 0xFF;
                        dst[col * 4 + 1] = 0xFF;
                        dst[col * 4 + 2] = 0xFF;
                        dst[col * 4 + 3] = CoverageGamma(cov);
                    }
                }
            }
            penX += (int)gm.gmCellIncX;
        }
    }

    if (!outlined && !pixelFilled) {
        // Fallback, and the mode font.outline_raster = off selects: GDI's own bitmap. The
        // coverage is the red channel here because the DC renders grayscale; it has to be
        // read before the channels are overwritten, or every glyph becomes a solid block.
        ::memset(bits, 0, (size_t)w * h * 4);
        RECT r = { 2, 2, w, h };
        ::DrawTextW(dc, draw.c_str(), (int)draw.size(), &r, DT_SINGLELINE | DT_NOPREFIX);
        BYTE* p = (BYTE*)bits;
        for (int i = 0; i < w * h; i++, p += 4) {
            const BYTE cov = p[2];
            p[0] = 0xFF; p[1] = 0xFF; p[2] = 0xFF;
            p[3] = CoverageGamma(cov);
        }
    }

    LPDIRECT3DTEXTURE9 tex = nullptr;
    HRESULT hr = dev->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_MANAGED, &tex, nullptr);
    if (SUCCEEDED(hr) && tex) {
        D3DLOCKED_RECT lr = {};
        if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
            for (int y = 0; y < h; y++) {
                ::memcpy((BYTE*)lr.pBits + (size_t)y * lr.Pitch,
                         (BYTE*)bits + (size_t)y * w * 4, (size_t)w * 4);
            }
            tex->UnlockRect(0);
        }

        LogDbg(L"texture built: px=%d %dx%d chars=%d weight=%d face='%s'",
               pxHeight, w, h, (int)text.size(), g_atlasWeight, FaceOf(fontId).c_str());
    } else {
        LogDbg(L"texture creation failed: px=%d %dx%d hr=0x%08X", pxHeight, w, h, (unsigned)hr);
    }

    ::SelectObject(dc, oldBmp);
    ::DeleteObject(bmp);
    ::SelectObject(dc, oldFont);
    ::DeleteObject(fn);
    ::DeleteDC(dc);

    if (!tex) return nullptr;
    ::InterlockedIncrement(&g_textureBuilds);
    if (outW) *outW = w;
    if (outH) *outH = h;
    // The advance is the measured width, not the texture's: the texture carries 2 px of padding
    // on each side, and advancing by the image put 4 px of blank between every pair of runs in a
    // line the client drew as one call with colour codes in it.
    if (outLogicalW) *outLogicalW = sz.cx;
    return tex;
}

// ---------------------------------------------------------------------------
LPDIRECT3DTEXTURE9 GetTextTexture(IDirect3DDevice9* dev, const std::wstring& text, int pxHeight,
                                  int fontId, int* outW, int* outH, int* outLogicalW)
{
    if (!dev || text.empty()) return nullptr;

    // The measurement caches are static maps touched from the render thread and from
    // the detours, so they take the same lock the texture cache uses.
    TexGuard guard;
    wchar_t prefix[32];
    swprintf_s(prefix, L"%d.%d.%d|", fontId, pxHeight, g_atlasWeight);
    std::wstring key = prefix + text;

    auto it = g_texCache.find(key);
    if (it != g_texCache.end() && it->second.tex) {
        it->second.lastUsed = ::GetTickCount();     // keep what a still frame keeps asking for
        if (outW) *outW = it->second.w;
        if (outH) *outH = it->second.h;
        if (outLogicalW) *outLogicalW = it->second.logicalW;
        return it->second.tex;
    }

    int w = 0, h = 0, logicalW = 0;
    LPDIRECT3DTEXTURE9 tex = BuildTexture(dev, text, pxHeight, fontId, &w, &h, &logicalW);
    if (!tex) return nullptr;

    // Budget the cache by texture memory, not by entry count: a single large
    // string can outweigh dozens of small ones.
    size_t bytes = (size_t)w * (size_t)h * 4;
    if (g_texBytes + bytes > kTexCacheBudget || g_texCache.size() >= kTexCacheEntries) {
        // Clearing everything made a server with one animated TextDraw (the moving site name in the
        // corner, a different string every frame) rebuild *every* string every frame: the new entry
        // pushed the cache over the budget, the whole cache went, and 35 pieces were rasterised
        // again - about a second per frame. Evict the least recently used entries instead: the
        // static lines are touched every frame and stay, the animated one ages out and is rebuilt
        // on its own.
        EvictOldest();
    }

    TexEntry e;
    e.tex = tex; e.w = w; e.h = h; e.logicalW = logicalW;
    e.lastUsed = ::GetTickCount();
    g_texCache[key] = e;
    g_texBytes += bytes;
    if (outW) *outW = w;
    if (outH) *outH = h;
    if (outLogicalW) *outLogicalW = logicalW;
    return tex;
}

// Natural line height for a face at this pixel size (cached).
int MeasureLineHeight(int pxHeight, int fontId)
{
    if (pxHeight <= 0) return 0;

    static std::unordered_map<long long, int> cache;
    TexGuard guard;
    long long key = ((long long)fontId << 32) | (long long)pxHeight;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    HDC dc = ::CreateCompatibleDC(nullptr);
    if (!dc) return pxHeight;
    HFONT fn = MakeFont(pxHeight, fontId);
    HGDIOBJ old = fn ? ::SelectObject(dc, fn) : nullptr;
    SIZE sz = { 0, 0 };
    ::GetTextExtentPoint32W(dc, L"Ag", 2, &sz);
    if (old) ::SelectObject(dc, old);
    if (fn) ::DeleteObject(fn);
    ::DeleteDC(dc);

    int h = sz.cy > 0 ? sz.cy : pxHeight;
    if (cache.size() >= kLineCacheEntries) {
        LogDbg(L"line-height cache full (%d entries), cleared", (int)cache.size());
        cache.clear();
    }
    cache[key] = h;
    return h;
}

// ---------------------------------------------------------------------------
// Which face can draw a character?
//
// The four display faces are Latin-only, so CJK (and any other character they
// do not carry) has to come from the fallback face. GetGlyphIndicesW reports
// the font's own cmap, which is what we want: relying on GDI's font linking
// would be invisible to us and different on every machine.
// ---------------------------------------------------------------------------
static int FontForChar(wchar_t ch, int fontId)
{
    if (fontId == kFontDefault || (unsigned)ch < 0x80) return fontId;

    static std::unordered_map<unsigned, unsigned char> cache;   // (font<<16|ch) -> covered
    TexGuard guard;
    unsigned key = ((unsigned)fontId << 16) | (unsigned)(ch & 0xFFFF);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second ? fontId : kFontDefault;

    bool covered = false;
    HDC dc = ::CreateCompatibleDC(nullptr);
    if (dc) {
        HFONT fn = MakeFont(48, fontId);
        HGDIOBJ old = fn ? ::SelectObject(dc, fn) : nullptr;
        WORD idx = 0;
        if (::GetGlyphIndicesW(dc, &ch, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR)
            covered = (idx != 0xFFFF);
        if (old) ::SelectObject(dc, old);
        if (fn) ::DeleteObject(fn);
        ::DeleteDC(dc);
    }

    if (cache.size() >= kGlyphCacheEntries) {
        LogDbg(L"glyph cache full (%d entries), cleared", (int)cache.size());
        cache.clear();
    }
    cache[key] = covered ? 1 : 0;
    return covered ? fontId : kFontDefault;
}

std::vector<TextSpan> SplitByFont(const std::wstring& in, int fontId)
{
    // TextDrawFont 2 is Bank Gothic, which has no lowercase glyphs: the original
    // renderer prints capitals, so "Style 3" shows up as "STYLE 3". Do the same
    // when a substitute face with lowercase letters is standing in for it.
    std::wstring s = in;
    if (fontId == 2) {
        for (size_t i = 0; i < s.size(); i++)
            if (s[i] >= L'a' && s[i] <= L'z') s[i] = (wchar_t)(s[i] - L'a' + L'A');
    }

    std::vector<TextSpan> out;
    if (s.empty()) return out;

    // A space is neutral: keep it on the face of the text before it. Otherwise
    // "你好 SAMP" would take the narrow Pricedown space between the two faces
    // and the gap would look different from a normal space.
    auto faceAt = [&](size_t i, int cur) {
        return (s[i] == L' ' || s[i] == L'\t') ? cur : FontForChar(s[i], fontId);
    };

    int cur = FontForChar(s[0], fontId);
    std::wstring acc;
    acc.push_back(s[0]);

    for (size_t i = 1; i < s.size(); i++) {
        int f = faceAt(i, cur);
        if (f != cur) {
            out.push_back(TextSpan{ acc, cur });
            acc.clear();
            cur = f;
        }
        acc.push_back(s[i]);
    }
    out.push_back(TextSpan{ acc, cur });
    return out;
}

// Drop the entries nothing has asked for in a while, oldest first, until the cache is back under
// half its budget. Called from GetTextTexture while the cache lock is held.
static void EvictOldest()
{
    if (g_texCache.empty()) return;
    const DWORD now = ::GetTickCount();

    struct Aged { std::wstring key; DWORD lastUsed; };
    std::vector<Aged> aged;
    aged.reserve(g_texCache.size());
    for (auto& kv : g_texCache) aged.push_back(Aged{ kv.first, kv.second.lastUsed });
    std::sort(aged.begin(), aged.end(),
              [](const Aged& a, const Aged& b) { return a.lastUsed < b.lastUsed; });

    size_t freed = 0, dropped = 0;
    const size_t target = kTexCacheBudget / 2;
    for (const Aged& a : aged) {
        if (g_texBytes <= target && g_texCache.size() < kTexCacheEntries / 2) break;
        auto it = g_texCache.find(a.key);
        if (it == g_texCache.end()) continue;
        const size_t bytes = (size_t)it->second.w * (size_t)it->second.h * 4;
        if (it->second.tex) it->second.tex->Release();
        g_texCache.erase(it);
        g_texBytes = (g_texBytes > bytes) ? g_texBytes - bytes : 0;
        freed += bytes; dropped++;
    }
    LogDbg(L"texture cache: evicted %u entries (%u KB), %u KB held now",
           (unsigned)dropped, (unsigned)(freed / 1024), (unsigned)(g_texBytes / 1024));
    (void)now;
}
void ClearTextureCache()
{
    TexGuard guard;
    for (auto& kv : g_texCache) {
        if (kv.second.tex) kv.second.tex->Release();
    }
    g_texCache.clear();
    g_texBytes = 0;
    ClearSymbolCache();
}

void SetTextureDevice(IDirect3DDevice9* dev)
{
    TexGuard guard;
    if (dev != g_texDevice) {
        if (g_texDevice) {
            ClearTextureCache();
            LogW(L"device changed (dev=%p), texture cache cleared", (void*)dev);
        }
        g_texDevice = dev;
    }
}




// ---------------------------------------------------------------------------
// The client's own arrow textures
//
// ~u~ ~d~ ~<~ ~>~ are not glyphs. CFont::PrintChar draws ButtonSprite[1..4],
// four CSprite2d that CFont::Initialise loads from MODELS\PCBTNS.TXD ("up",
// "down", "left", "right"), in a 17H x 17H square 2H below the pen, with a white
// vertex colour and the text alpha. CFont::ButtonSprite is at gta_sa+0xC71AD8 and
// each entry is only the sprite's RwTexture pointer.
//
// The layout was read out of CSprite2d::Draw (gta_sa 0x728350): the sprite holds
// the RwTexture at +0, the texture's raster is its first dword, and the raster
// keeps its size at +0x0C / +0x10. The D3D9 texture inside the raster sits at a
// private offset that depends on the RenderWare build.
//
// Probing that offset by calling GetLevelDesc on every candidate would raise one
// access violation per wrong candidate, so a candidate is only ever touched when
// its vtable is readable and lives in the same module as the other three sprites'
// vtables. That narrows it to a handful of candidates; only those are asked for
// their size, and the offset is accepted when the reported size is exactly what
// the raster itself says for all four sprites. If nothing matches,
// GetClientSymbolTexture returns null and the renderer draws the Unicode arrow.
// ---------------------------------------------------------------------------
static LPDIRECT3DTEXTURE9 g_symbolTex[5]  = {};   // eExtraFontSymbol 1..4
static DWORD              g_symbolLastTry = 0;
static int                g_symbolTries   = 0;

void ClearSymbolCache()
{
    for (int i = 0; i < 5; i++) g_symbolTex[i] = nullptr;
    g_symbolLastTry = 0;
    g_symbolTries   = 0;
}

static bool Committed(const void* p, size_t bytes)
{
    if (!p || (uintptr_t)p < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (::VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const uintptr_t end = (uintptr_t)p + bytes;
    return end <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static HMODULE ModuleOf(const void* p)
{
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)p, &mod)) return nullptr;
    return mod;
}

static bool ProbeClientSymbols()
{
    HMODULE gta = ::GetModuleHandleW(L"gta_sa.exe");
    if (!gta) return false;

    __try {
        // CFont::ButtonSprite is an absolute VA in the client's own code (the
        // game is linked at 0x400000 and its immediates are already absolute), so
        // the offset from the module base is 0xC71AD8 - 0x400000.
        BYTE** slots = (BYTE**)((BYTE*)gta + (0xC71AD8 - 0x400000));
        BYTE*  raster[5] = {};
        int    rw[5] = {}, rh[5] = {};
        for (int s = 1; s <= 4; s++) {
            BYTE* texture = slots[s];                       // CSprite2d::m_pTexture
            if (!Committed(texture, 4)) continue;
            raster[s] = *(BYTE**)texture;                   // RwTexture::raster
            if (!Committed(raster[s], 0x14)) continue;
            rw[s] = *(int*)(raster[s] + 0x0C);
            rh[s] = *(int*)(raster[s] + 0x10);
        }
        if (!raster[1] || !raster[2] || !raster[3] || !raster[4]) {
            LogW(L"symbols: ButtonSprite not ready (slots %p %p %p %p, raster %p %p %p %p)",
                 slots[1], slots[2], slots[3], slots[4], raster[1], raster[2], raster[3], raster[4]);
            return false;
        }

        for (int off = 0x14; off <= 0x80; off += 4) {
            void* cand[5] = {};
            HMODULE codeMod = nullptr;
            bool ok = true;
            for (int s = 1; s <= 4 && ok; s++) {
                if (rw[s] < 8 || rh[s] < 8 || rw[s] > 2048 || rh[s] > 2048) { ok = false; break; }
                void* p = *(void**)(raster[s] + off);
                if (!Committed(p, 0x20)) { ok = false; break; }
                void* vt = *(void**)p;                      // the COM vtable
                if (!Committed(vt, 0x40)) { ok = false; break; }
                HMODULE m = ModuleOf(vt);
                if (!m) { ok = false; break; }
                if (!codeMod) codeMod = m;
                else if (m != codeMod) { ok = false; break; }
                cand[s] = p;
            }
            if (!ok) continue;

            int hits = 0, w[5] = {}, h[5] = {};
            for (int s = 1; s <= 4; s++) {
                D3DSURFACE_DESC d;
                if (FAILED(((IDirect3DTexture9*)cand[s])->GetLevelDesc(0, &d))) continue;
                if ((int)d.Width != rw[s] || (int)d.Height != rh[s]) continue;
                w[s] = (int)d.Width; h[s] = (int)d.Height; hits++;
            }
            if (hits < 4) continue;
            for (int s = 1; s <= 4; s++) g_symbolTex[s] = (IDirect3DTexture9*)cand[s];
            LogW(L"symbols: client textures at raster+0x%X (%dx%d %dx%d %dx%d %dx%d)",
                 off, w[1], h[1], w[2], h[2], w[3], h[3], w[4], h[4]);
            return true;
        }
        LogW(L"symbols: no D3D9 texture inside the raster (sizes %dx%d and %dx%d), using Unicode arrows",
             rw[1], rh[1], rw[3], rh[3]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogW(L"symbols: reading the client's sprites raised, using Unicode arrows");
    }
    return false;
}

LPDIRECT3DTEXTURE9 GetClientSymbolTexture(IDirect3DDevice9* dev, int symbol)
{
    (void)dev;   // the texture belongs to the client's device, which is ours too
    if (symbol < 1 || symbol > 4) return nullptr;

    // The cached object belongs to the client. Asking for its size again costs
    // one call and catches a device reset or an unloaded texture before the
    // pointer reaches SetTexture; if it is gone, drop everything and re-probe.
    if (g_symbolTex[symbol]) {
        __try {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(g_symbolTex[symbol]->GetLevelDesc(0, &d)))
                return g_symbolTex[symbol];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        ClearSymbolCache();
        return nullptr;
    }
    if (g_symbolTries >= 8) return nullptr;

    // The very first arrow can be drawn before CFont::Initialise has finished
    // loading PCBTNS.TXD, so retry about once a second instead of giving up.
    const DWORD now = ::GetTickCount();
    if (g_symbolLastTry && now - g_symbolLastTry < 1000) return nullptr;
    g_symbolLastTry = now;
    g_symbolTries++;
    ProbeClientSymbols();
    return g_symbolTex[symbol];
}

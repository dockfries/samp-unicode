// ---------------------------------------------------------------------------
// render.cpp - vtable hooks on Present / BeginScene / EndScene / Reset + quad rendering



// Render state sequence, vertex format and blend mode for the text quads.
// ---------------------------------------------------------------------------
#include "common.h"
#include "gametext_styles.h"
#include <cstdio>

IDirect3DDevice9* g_device = nullptr;

// State needed to put the device vtable back when the plugin unloads.
static void** g_vtSlots      = nullptr;
static void*  g_prevReset    = nullptr;
bool          g_overlayReady = false;   // the frame hook (Present or EndScene) is live

static std::vector<DrawItem>     g_items;
static std::deque<GameTextItem>  g_gametexts;
static CRITICAL_SECTION          g_listCs;
static std::atomic<bool>         g_listCsInit{ false };

int GameTextStyleCount() { return kGtStyleCount; }

// game functions
typedef HRESULT(__stdcall* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(__stdcall* BeginScene_t)(IDirect3DDevice9*);
typedef HRESULT(__stdcall* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static Present_t    g_origPresent    = nullptr;
static void*        g_prevPresent    = nullptr;
static BeginScene_t g_origBeginScene = nullptr;
static void*        g_prevBeginScene = nullptr;
static Reset_t      g_origReset      = nullptr;


// Vertex format: D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1 = 0x144
struct TLVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
    float u, v;
};
static const DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// ---------------------------------------------------------------------------
static void ListLock()
{
    // std::call_once: two threads can arrive here at the same time during startup.
    static std::once_flag once;
    std::call_once(once, [] { ::InitializeCriticalSection(&g_listCs); g_listCsInit = true; });
    ::EnterCriticalSection(&g_listCs);
}
static void ListUnlock() { ::LeaveCriticalSection(&g_listCs); }

void PushDrawItem(const DrawItem& item)
{
    ListLock();
    if (g_items.size() >= 128) {         // per-frame item cap
        ListUnlock();
        return;
    }
    g_items.push_back(item);
    ListUnlock();
}


// True when this exact string is one we already draw from the GameText path. The
// client feeds its GameText through a TextDraw as well, so without this the same
// message is drawn twice: once by the GameText layout and once by the TextDraw
// hook, at the textdraw's own size and position.
// Menu text is drawn from the CFont::PrintString path, but the client also puts the
// title into a TextDraw, which our TextDraw hook then draws a second time at almost
// the same place - the doubled "menu" title. Positions are remembered here (in
// pixels, same space the TextDraw hook works in) so that copy can be skipped.
struct MenuNote { float x, y; int px; DWORD tick; };
static MenuNote g_menuNotes[32];
static int      g_menuNoteCount = 0;

void NoteMenuDraw(float x, float y, int px)
{
    ListLock();
    const DWORD now = ::GetTickCount();
    if (g_menuNoteCount >= (int)(sizeof(g_menuNotes) / sizeof(g_menuNotes[0])) ||
        now - (g_menuNoteCount ? g_menuNotes[g_menuNoteCount - 1].tick : now) > 500) {
        g_menuNoteCount = 0;
    }
    g_menuNotes[g_menuNoteCount].x = x;
    g_menuNotes[g_menuNoteCount].y = y;
    g_menuNotes[g_menuNoteCount].px = px;
    g_menuNotes[g_menuNoteCount].tick = now;
    g_menuNoteCount++;
    ListUnlock();
}

// The menu title's text, so the client's TextDraw copy of it can be dropped before the
// position note exists (see MenuNoteTitle). One menu at a time, so one entry is enough.
static std::wstring g_menuTitleText;
static DWORD        g_menuTitleTick = 0;

void NoteMenuTitle(const std::wstring& text)
{
    if (text.empty()) return;
    ListLock();
    g_menuTitleText = text;
    g_menuTitleTick = ::GetTickCount();
    ListUnlock();
}

bool IsMenuTitleText(const std::wstring& text)
{
    if (text.empty()) return false;
    bool hit = false;
    ListLock();
    if (!g_menuTitleText.empty() && g_menuTitleText == text &&
        (::GetTickCount() - g_menuTitleTick) <= 500) {
        hit = true;
    }
    ListUnlock();
    return hit;
}

bool IsMenuDuplicate(float x, float y, int px)
{
    bool hit = false;
    ListLock();
    const DWORD now = ::GetTickCount();
    for (int i = 0; i < g_menuNoteCount; i++) {
        const MenuNote& n = g_menuNotes[i];
        if (now - n.tick > 500) continue;
        if (px != n.px) continue;
        const float dx = x - n.x, dy = y - n.y;
        if (dx > -8.0f && dx < 8.0f && dy > -26.0f && dy < 26.0f) { hit = true; break; }
    }
    ListUnlock();
    return hit;
}

bool IsGameTextText(const std::wstring& s)
{
    bool hit = false;
    ListLock();
    for (size_t i = 0; i < g_gametexts.size(); i++) {
        if (g_gametexts[i].text == s) { hit = true; break; }
    }
    ListUnlock();
    return hit;
}

// The client has a single GameText slot per player: a new message replaces whatever
// was showing, whatever its style. Keeping a queue per style stacked the layouts on
// top of each other (a style 2 message drawn over a still running style 0).
void ClearGameTextAll()
{
    ListLock();
    g_gametexts.clear();
    ListUnlock();
}


void PushGameText(const std::wstring& text, int style, int timeMs)
{
    if (style < 0 || style >= kGtStyleCount) style = 1;
    if (timeMs < 0) timeMs = 0;

    ListLock();
    // One slot, like the client: the new message replaces whatever was showing.

    g_gametexts.clear();   // one slot: a new message replaces the previous one
    GameTextItem it;
    it.text = text;
    it.style = style;
    it.start = ::GetTickCount();
    it.timeMs = timeMs;
    g_gametexts.push_back(it);
    ListUnlock();
}

// ---------------------------------------------------------------------------
// Render states: save -> set -> restore
// ---------------------------------------------------------------------------

// The device state we have to put back is not a fixed list. SetupState below
// changes render states, texture-stage states, sampler states, the vertex and
// pixel shader and the FVF, and RenderWare caches all of those internally: if we
// leave them changed it may skip re-setting its own values and submit the next
// frame's world geometry with our 2D FVF and no shader. A D3DSBT_ALL state block
// captures everything and restores it in one call.
static IDirect3DStateBlock9* g_stateBlock = nullptr;
static IDirect3DDevice9*     g_stateBlockDev = nullptr;

static bool StateBlockCapture(IDirect3DDevice9* dev)
{
    if (!g_stateBlock) {
        // Recreated whenever it is missing, which is also what happens after a
        // Reset (see DropStateBlock): a block captured for the previous backbuffer
        // holds the old viewport and restoring it distorts the frame.
        if (g_stateBlockDev != dev) {
            g_stateBlockDev = dev;
        }
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_stateBlock))) {
            g_stateBlock = nullptr;
            g_stateBlockDev = nullptr;
            LogW(L"CreateStateBlock failed, text overlay disabled for this device");
            return false;
        }
    }
    return g_stateBlock && SUCCEEDED(g_stateBlock->Capture());
}

static void StateBlockApply()
{
    if (g_stateBlock) g_stateBlock->Apply();
}

// Drop the cached block so the next frame builds one for the new backbuffer.
static void DropStateBlock()
{
    if (g_stateBlock) { g_stateBlock->Release(); g_stateBlock = nullptr; }
    g_stateBlockDev = nullptr;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Background store for the overlay
//
// While a menu (or the pause menu) is up the client keeps presenting the same frame
// and repaints only the menu's own rectangle, so whatever the overlay stops drawing
// stays on screen. Putting the background back is the only way to remove it.
//
// A multisampled render target cannot be copied to a plain surface with StretchRect
// (D3DERR_INVALIDCALL, which is what the log showed), so the capture resolves into a
// non-multisampled render target and pulls that with GetRenderTargetData. The restore
// draws the stored pixels back as a textured quad: drawing into a multisampled target
// is allowed, StretchRect into it is not.
// ---------------------------------------------------------------------------
static IDirect3DSurface9* g_bgResolve = nullptr;   // non-MSAA render target (capture)
static IDirect3DSurface9* g_bgSys     = nullptr;   // system memory copy
static IDirect3DTexture9* g_bgTex     = nullptr;   // sampled when restoring
static int       g_bgW = 0, g_bgH = 0;
static D3DFORMAT g_bgFmt = D3DFMT_UNKNOWN;
static RECT      g_bgRect = { 0, 0, 0, 0 };
static bool      g_bgValid = false;

// Set by the menu path every time it draws a line, consumed at Present. MenuWindowActive()
// keeps a 3 s window after the last CMenu::Show, which is fine for "is a menu open" but
// wrong for "is the frame frozen": erasing on a live frame shows a stale patch.
static std::atomic<long> g_menuDraws{ 0 };

void NoteMenuFrame() { g_menuDraws.fetch_add(1, std::memory_order_relaxed); }

bool MenuDrawnThisFrame()
{
    return g_menuDraws.exchange(0, std::memory_order_relaxed) > 0;
}

// True when the frame that is currently on screen was drawn by the menu. BeginScene
// runs before the game draws, so this is how the overlay knows the pixels behind the
// text are frozen and can be restored without flashing a stale patch over a live one.
static bool g_menuFramePrev = false;

static void BgDrop()
{
    if (g_bgResolve) { g_bgResolve->Release(); g_bgResolve = nullptr; }
    if (g_bgSys)     { g_bgSys->Release();     g_bgSys = nullptr; }
    if (g_bgTex)     { g_bgTex->Release();     g_bgTex = nullptr; }
    g_bgW = g_bgH = 0;
    g_bgFmt = D3DFMT_UNKNOWN;
    g_bgRect = RECT{ 0, 0, 0, 0 };
    g_bgValid = false;
}

static bool BgEnsure(IDirect3DDevice9* dev, int w, int h, D3DFORMAT fmt)
{
    if (g_bgResolve && g_bgSys && g_bgTex && w <= g_bgW && h <= g_bgH && g_bgFmt == fmt)
        return true;

    BgDrop();
    if (FAILED(dev->CreateRenderTarget((UINT)w, (UINT)h, fmt, D3DMULTISAMPLE_NONE, 0,
                                       FALSE, &g_bgResolve, nullptr))) {
        BgDrop();
        return false;
    }
    if (FAILED(dev->CreateOffscreenPlainSurface((UINT)w, (UINT)h, fmt, D3DPOOL_SYSTEMMEM,
                                                &g_bgSys, nullptr))) {
        BgDrop();
        return false;
    }
    if (FAILED(dev->CreateTexture((UINT)w, (UINT)h, 1, 0, fmt, D3DPOOL_DEFAULT, &g_bgTex, nullptr))) {
        BgDrop();
        return false;
    }
    g_bgW = w;
    g_bgH = h;
    g_bgFmt = fmt;
    return true;
}

// Copy the area the overlay is about to draw on, before drawing on it.
static bool BgCapture(IDirect3DDevice9* dev, const RECT& r)
{
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return false;

    D3DSURFACE_DESC rd = {};
    if (FAILED(rt->GetDesc(&rd)) || !rd.Width) { rt->Release(); return false; }

    // Stay inside the target: text can sit partly off screen, and a rectangle that
    // crosses the edge makes StretchRect fail with no copy at all.
    RECT c = r;
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;
    if (c.right > (LONG)rd.Width) c.right = (LONG)rd.Width;
    if (c.bottom > (LONG)rd.Height) c.bottom = (LONG)rd.Height;

    const int w = c.right - c.left;
    const int h = c.bottom - c.top;
    if (w <= 0 || h <= 0) { rt->Release(); return false; }

    if (!BgEnsure(dev, w, h, rd.Format)) {
        rt->Release();
        static bool onceFmt = false;
        if (!onceFmt) {
            onceFmt = true;
            LogW(L"background surfaces could not be created (fmt=%d %dx%d)", (int)rd.Format, w, h);
        }
        return false;
    }

    const RECT dst = { 0, 0, w, h };
    HRESULT hr = dev->StretchRect(rt, &c, g_bgResolve, &dst, D3DTEXF_NONE);
    rt->Release();
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(g_bgResolve, g_bgSys);
    if (FAILED(hr)) {
        static bool onceErr = false;
        if (!onceErr) {
            onceErr = true;
            LogW(L"background copy failed (%08X) %dx%d at %ld,%ld (msaa=%d fmt=%d)",
                 (unsigned)hr, w, h, c.left, c.top, (int)rd.MultiSampleType, (int)rd.Format);
        }
        return false;
    }

    g_bgRect = c;
    g_bgValid = true;
    return true;
}

// Draw the stored background back over the rectangle the overlay used.
static void BgErase(IDirect3DDevice9* dev)
{
    if (!g_bgValid || !g_bgSys || !g_bgTex) return;

    IDirect3DSurface9* tex = nullptr;
    if (FAILED(g_bgTex->GetSurfaceLevel(0, &tex)) || !tex) return;
    const HRESULT hr = dev->UpdateSurface(g_bgSys, nullptr, tex, nullptr);
    tex->Release();
    if (FAILED(hr)) {
        static bool onceUpd = false;
        if (!onceUpd) { onceUpd = true; LogW(L"background restore failed (%08X)", (unsigned)hr); }
        return;
    }

    // The surfaces are grow-only, so the stored rectangle is a sub-rect of a texture
    // that may be larger: sampling 0..1 would stretch the stale rest of it over the
    // rectangle.
    const float x0 = (float)g_bgRect.left, y0 = (float)g_bgRect.top;
    const float x1 = (float)g_bgRect.right, y1 = (float)g_bgRect.bottom;
    const float u1 = g_bgW > 0 ? (float)(g_bgRect.right - g_bgRect.left) / (float)g_bgW : 1.0f;
    const float v1 = g_bgH > 0 ? (float)(g_bgRect.bottom - g_bgRect.top) / (float)g_bgH : 1.0f;
    const TLVertex v[4] = {
        { x0, y0, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { x1, y0, 0.0f, 1.0f, 0xFFFFFFFF, u1,   0.0f },
        { x1, y1, 0.0f, 1.0f, 0xFFFFFFFF, u1,   v1   },
        { x0, y1, 0.0f, 1.0f, 0xFFFFFFFF, 0.0f, v1   },
    };
    dev->SetTexture(0, g_bgTex);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, v, sizeof(TLVertex));
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetTexture(0, nullptr);
}
// Draw one item (outline -> shadow -> body)
// ---------------------------------------------------------------------------
static void SetupState(IDirect3DDevice9* dev)
{
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    // Point sampling, like D3DXFont: the glyph atlas is a 1-bit stencil, and a linear filter
    // turns every hard edge into a four pixel grey ramp. That ramp was the "haziness" that no
    // font quality or gamma setting could remove, because the glyphs themselves were already
    // hard - the filter was doing the smoothing at composite time.
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(kFvf);
}

// GTA draws its text in a 640x480 space and stretches the outline and the shadow by the real
// screen (SCREEN_STRETCH_X/Y, gta-reversed source/game_sa/common.h), so the client's outline=1 is
// four pixels wide at 2560 and one at 640. DrawOneItem only sees device pixels, so the current
// screen is kept here and the TextDraw/GameText offsets are scaled the same way.
static int g_stretchW = 640;
static int g_stretchH = 480;
// GTA's own scaling: SCREEN_STRETCH_X divides by 640 and SCREEN_STRETCH_Y by 448
// (gta-reversed source/game_sa/common.h DEFAULT_SCREEN_WIDTH/HEIGHT) and neither is bounded -
// the field is a byte, so 255 really is 1020 px at 2560 wide, and that is what the client draws.
static int StretchX(int n) { const int v = (int)((float)n * (float)g_stretchW / 640.0f + 0.5f); return v < 1 ? 1 : v; }
static int StretchY(int n) { const int v = (int)((float)n * (float)g_stretchH / 448.0f + 0.5f); return v < 1 ? 1 : v; }

static void DrawOneItem(IDirect3DDevice9* dev, const DrawItem& it, bool bodyPass)
{
    // An inline client symbol (the arrows) is one square quad with the client's
    // own texture, white vertex colour and the text alpha - exactly what
    // CFont::PrintChar does. It has no outline and no shadow; the client returns
    // right after drawing it. it.px carries the square size here.
    if (it.sprite) {
        if (!bodyPass) return;
        LPDIRECT3DTEXTURE9 sym = GetClientSymbolTexture(dev, it.sprite);
        if (sym) {
            const float sz = (float)it.px;
            const D3DCOLOR col = (it.color & 0xFF000000u) | 0x00FFFFFFu;   // white, text alpha
            TLVertex v[6];
            const TLVertex a = { it.x,      it.y,      0.0f, 1.0f, col, 0.0f, 0.0f };
            const TLVertex b = { it.x + sz, it.y,      0.0f, 1.0f, col, 1.0f, 0.0f };
            const TLVertex c = { it.x + sz, it.y + sz, 0.0f, 1.0f, col, 1.0f, 1.0f };
            const TLVertex d = { it.x,      it.y + sz, 0.0f, 1.0f, col, 0.0f, 1.0f };
            v[0] = a; v[1] = b; v[2] = c; v[3] = a; v[4] = c; v[5] = d;
            dev->SetTexture(0, sym);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(TLVertex));
            return;
        }
        // no texture: fall through and draw the Unicode arrow kept in it.text
    }

    int w = 0, h = 0;
    SetAtlasWeight(it.weight);
    LPDIRECT3DTEXTURE9 tex = GetTextTexture(dev, it.text, it.px, it.fontId, &w, &h);
    if (!tex) return;

    float qw = (float)w * it.scaleX;      // letterWidth stretch
    float x  = it.x;
    if (it.right)       x -= qw;
    else if (it.center) x -= qw * 0.5f;
    float y = it.y;

    // A scaled run cannot be point sampled: nearest neighbour copies whole glyph columns, which
    // is what made the scoreboard's stretched title look blocky beside the very same text drawn
    // 1:1. Linear is safe now that the atlas carries real grayscale coverage - it only smooths
    // the enlargement instead of inventing an edge ramp the glyph does not have.
    const bool scaled = (it.scaleX < 0.999f || it.scaleX > 1.001f);
    const D3DTEXTUREFILTERTYPE filter = scaled ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);

    dev->SetTexture(0, tex);

    // One draw call per item: every outline ring, the shadow and the body go into
    // one vertex buffer. A DrawPrimitive per quad cost 8n per ring and multiplied
    // by every run a mixed line splits into (thousands of calls per frame for a
    // thick outline).
    std::vector<TLVertex> verts;
    // Never reserve from a raw signed field: (size_t)(-1) * 8 wraps to a huge value and
    // reserve() throws length_error, which used to cost the item its whole draw.
    const int outlineQuads = (it.outlineSize > 0 && it.outlineSize <= 4096) ? it.outlineSize : 0;
    verts.reserve(6 * (size_t)(1 + (size_t)outlineQuads * 8));

    auto quad = [&verts](float qx, float qy, float qw2, float qh2, D3DCOLOR col) {
        const TLVertex a = { qx,        qy,        0.0f, 1.0f, col, 0.0f, 0.0f };
        const TLVertex b = { qx + qw2,  qy,        0.0f, 1.0f, col, 1.0f, 0.0f };
        const TLVertex c = { qx + qw2,  qy + qh2,  0.0f, 1.0f, col, 1.0f, 1.0f };
        const TLVertex d = { qx,        qy + qh2,  0.0f, 1.0f, col, 0.0f, 1.0f };
        verts.push_back(a); verts.push_back(b); verts.push_back(c);
        verts.push_back(a); verts.push_back(c); verts.push_back(d);
    };

    // SA-MP stores outline/shadow as sizes, not flags: draw that many rings /
    // that many pixels of offset, so outline=2 and shadow=2 look thicker instead
    // of being clamped to one pixel.
    // Two passes: every outline and shadow is drawn before any body, otherwise a
    // run's outline (which reaches outlineSize pixels to the left) covers the last
    // glyph of the run before it. Mixed Latin/CJK lines are split into many runs,
    // so that used to eat up to 2*outlineSize pixels of the previous run.
    if (!bodyPass) {
        // For a TextDraw the client draws an outline *or* a shadow, never both:
        // textdraw.cpp:219 is "if (outline) SetOutline else SetShadow", and GTA's PrintString
        // runs the outline branch only while DropAmount is zero. A TextDraw therefore has a
        // shadow only when the server asked for one - [font] shadow is the fallback for the
        // surfaces this mod lays out itself, not for a draw the client would have left bare.
        //
        // Menu and GameText lines are this mod's own: [menu] outline/shadow and the GameText
        // style table set both on purpose and both are drawn, so the rule above must not touch
        // them (menu.outline=1 with menu.shadow=3 is the default, and losing one of the two is
        // a visible change).
        const bool hasOutline = it.outlineSize > 0;
        const bool shadowSuppressed = it.isTextDraw && hasOutline;
        const bool wantShadow = it.shadowSize > 0 || (g_cfg.shadow && !it.isTextDraw);
        if (!shadowSuppressed && wantShadow) {
            int sx = it.shadowSize > 0 ? it.shadowSize : g_cfg.shadowOffsetX;
            int sy = it.shadowSize > 0 ? it.shadowSize : g_cfg.shadowOffsetY;
            // Only the client's own amount is in the 640x480 space; the [font] fallback is pixels.
            if (it.shadowSize > 0 && it.stretchOutline) {
                sx = StretchX(sx);
                sy = StretchY(sy);
            }
            quad(x + (float)sx, y + (float)sy, qw, (float)h, it.shadowColor);
        }
        if (it.outlineSize > 0) {
            // Eight offsets at the outline distance - exactly what GTA itself does. Its
            // CFont::PrintString ("Handle outline", gta-reversed source/game_sa/Font.cpp) draws the
            // whole string eight times, at (+-n,0), (0,+-n) and the four diagonals, with the edge
            // amount zeroed so those passes do not recurse; the body is drawn afterwards on top.
            // A full ring instead (sum of 8r, i.e. 4n(n+1) quads of the whole run) is not the
            // original shape - at n=2 it fills pixels GTA leaves empty - and at the 255 an unsigned
            // read of -1 produced it was nine million quads per frame.
            static const int kDirs[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 },
                                             { 1, 0 },   { -1, 1 }, { 0, 1 },  { 1, 1 } };
            // The amount is in the client's 640x480 space, so it is stretched like GTA does it:
            // outline=1 is one pixel at 640x480 and four at 2560 wide.
            const int ox = it.stretchOutline ? StretchX(it.outlineSize) : it.outlineSize;
            const int oy = it.stretchOutline ? StretchY(it.outlineSize) : it.outlineSize;
            for (int i = 0; i < 8; i++)
                quad(x + (float)(kDirs[i][0] * ox), y + (float)(kDirs[i][1] * oy), qw,
                     (float)h, it.color2);
        }
        // Client text is not ringed: the client paints its shadow as the same line copied
        // a few pixels around, and those offsets are not a ring. Four copies in a cross up
        // to 1280 px wide, and eight - the four diagonals at one pixel plus the four
        // straight offsets at two - above it. Reproducing them exactly is what makes the
        // outline the same weight as the original instead of merely black.
        // 1 = one copy at the configured offset, which is the shadow DXUT puts under a
        // dialog title or a button caption (the client draws that as a translucent black
        // pass, and this replaces the pass we dropped).
        if (it.shadowCopies == 1) {
            quad(x + (float)g_cfg.shadowOffsetX, y + (float)g_cfg.shadowOffsetY, qw, (float)h,
                 it.color2);
        } else if (it.shadowCopies > 0) {
            static const int kCross[4][2] = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
            static const int kWide[8][2]  = { { 0, -2 }, { -1, -1 }, { 1, -1 }, { 0, 2 },
                                              { -1, 1 }, { 1, 1 },  { -2, 0 }, { 2, 0 } };
            const int n = (it.shadowCopies >= 8) ? 8 : 4;
            const int (*off)[2] = (it.shadowCopies >= 8) ? kWide : kCross;
            for (int i = 0; i < n; i++)
                quad(x + (float)off[i][0], y + (float)off[i][1], qw, (float)h, it.color2);
        }
    } else {
        // The client's own UI text forces its body opaque: color | 0xff000000 in
        // CChat::RenderEntry (chatwindow.cpp:624 of the decompiled source). Only that path does:
        // a TextDraw and a GameText carry their own alpha, and it is what their fade animates, so
        // forcing it there made every faded line start at full strength.
        quad(x, y, qw, (float)h, it.forceOpaque ? (it.color | 0xFF000000u) : it.color);
    }

    if (!verts.empty()) {
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(verts.size() / 3), verts.data(), sizeof(TLVertex));
    }
}

// One bad item must not take the frame with it: an exception here used to skip the whole
// overlay, and since the TextDraw the client would have drawn is already blanked by the time we
// draw, its text simply disappeared. Draw each item under its own guard and say which one threw.
static void SafeDrawItem(IDirect3DDevice9* dev, const DrawItem& it, bool bodyPass)
{
    __try {
        DrawOneItem(dev, it, bodyPass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool once = false;
        if (!once) {
            once = true;
            LogW(L"draw item threw: pass=%d px=%d outline=%d shadow=%d '%s'",
                 (int)bodyPass, it.px, it.outlineSize, it.shadowSize, it.text.c_str());
        }
    }
}

// GameText fades in and out on the client; mirror that so a redrawn message
// does not pop in at full alpha and vanish instantly. A permanent message
// (time 0, which the fixes treat as "until replaced") only fades in.
static DWORD FadeLength(DWORD total)
{
    DWORD fade = 400;
    if (total > 0 && fade > total / 3) fade = total / 3;
    if (fade < 60) fade = 60;
    return fade;
}

static float FadeAlpha(DWORD elapsed, DWORD total, bool fadeOut)
{
    (void)total;

    // Fade in only. A fade *out* is drawn on top of the previous frame's brighter
    // pixels, and alpha blending cannot remove them: while the frame behind is not
    // repainted (menu open, pause menu), each step leaves a dimmer copy that stays
    // there after the message is gone. Cutting at full brightness removes the text
    // cleanly, which is what the no-fade comparison build showed.
    DWORD fade = FadeLength(total);
    float a = 1.0f;
    if (elapsed < fade) a = (float)elapsed / (float)fade;
    else if (fadeOut && total > 0 && elapsed + fade > total) {
        const DWORD left = (elapsed < total) ? (total - elapsed) : 0;
        a = (float)left / (float)fade;
    }
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return a;
}

// ---------------------------------------------------------------------------
// overlay main flow (Present, or EndScene when hook_point says so)
// ---------------------------------------------------------------------------
// GTA stops drawing the game world (and every TextDraw) while the pause menu or a
// cutscene holds the game, but our own quads are emitted unconditionally from
// the frame, so a GameText would float on top of the menu. These are the flags the
// game itself uses: CTimer::m_UserPause and CTimer::m_CodePause.
static DWORD g_lastFrameTick = 0;


static bool IsGamePaused()
{
    const volatile BYTE* userPause = (const volatile BYTE*)0x00B7CB48;
    const volatile BYTE* codePause = (const volatile BYTE*)0x00B7CB49;
    return *userPause != 0 || *codePause != 0;
}

// Build the GameText layer into 'gts' (and grow 'gtRect'). With underPanel the
// layer is collected even while the scoreboard is open: the scoreboard detour
// draws it before the panel, and the panel is translucent, so the text shows
// through exactly like the client's own GameText does. The caller holds the list
// lock.
static void BuildGameTextLayer(std::vector<DrawItem>& gts, RECT& gtRect, bool& gtRectValid,
                               DWORD now, int screenW, int screenH, bool underPanel)
{
    const bool scoreboardUp = ScoreboardUp();   // one read per frame, not per item
    for (size_t i = 0; i < g_gametexts.size();) {
        GameTextItem& gt = g_gametexts[i];
        DWORD elapsed   = now - gt.start;
        bool  permanent = (gt.timeMs <= 0);
        if (!permanent && elapsed >= (DWORD)gt.timeMs) {
            g_gametexts.erase(g_gametexts.begin() + i);
            continue;
        }
        if (gt.style < 0 || gt.style >= kGtStyleCount) {
            g_gametexts.erase(g_gametexts.begin() + i);
            continue;
        }

        // The scoreboard covers the stock GameText; do not draw ours over it.
        // The item stays in the list, so the timer keeps running.
        if (!underPanel && scoreboardUp) {
            i++;
            continue;
        }

        const GtStyle& st = kGtStyles[gt.style];

        // Same scale unit as a TextDraw letter height, so the two paths agree
        // when size_divisor is tuned; gameTextScale nudges GameText on its own.
        int px = (int)(st.letterH * (float)screenH / g_cfg.sizeDivisor * g_cfg.gameTextScale + 0.5f);
        if (px < 6)   px = 6;
        if (px > 200) px = 200;

        DWORD total = permanent ? 0 : (DWORD)gt.timeMs;
        float fade  = FadeAlpha(elapsed, total, g_cfg.gameTextFadeOut);
        BYTE  alpha = (BYTE)(fade * 255.0f + 0.5f);

        // GameText carries the same colour codes as a TextDraw (~r~, {RRGGBB})
        // and ~n~ line breaks, so it needs the same run splitting; the base
        // colour is the style's own colour, not white, and ~h~ lightens that.
        // Codes the client does not know are dropped there (see ParseColorRuns
        // for ~]~, which the GameText reference documents but the client ignores).
        std::vector<TextRun> runs = ParseColorRuns(gt.text, st.color, g_cfg.braceColors);

        struct GtLine { std::vector<TextRun> runs; };
        std::vector<GtLine> lines(1);
        for (size_t r = 0; r < runs.size(); r++) {
            std::vector<std::wstring> parts = SplitLines(runs[r].text);
            for (size_t k = 0; k < parts.size(); k++) {
                if (k > 0) lines.push_back(GtLine());
                if (!parts[k].empty()) lines.back().runs.push_back(TextRun{ parts[k], runs[r].color });
            }
        }

        float y     = st.y * (float)screenH / 448.0f + g_cfg.baselineOffset;
        int   lineH = MeasureLineHeight(px, st.font);

        // Split by face as well: st.font is the TextDrawFont face the fix uses
        // for this style, and CJK falls back to the display font.
        struct Piece { std::wstring text; D3DCOLOR color; int font; int width; int symbol; };

        for (size_t li = 0; li < lines.size(); li++) {
            std::vector<TextRun>& line = lines[li].runs;

            std::vector<Piece> pieces;
            int lineW = 0;
            for (size_t k = 0; k < line.size(); k++) {
                std::vector<DrawPiece> dp;
                SplitRunIntoPieces(line[k].text, px, st.font, dp);
                for (size_t s = 0; s < dp.size(); s++) {
                    pieces.push_back(Piece{ dp[s].text, line[k].color, dp[s].font, dp[s].width, dp[s].symbol });
                    lineW += dp[s].width;
                }
            }

            float x = st.x * (float)screenW / 640.0f;
            if (st.align == 2)      x -= (float)lineW;
            else if (st.align == 1) x -= (float)lineW * 0.5f;

            for (size_t k = 0; k < pieces.size(); k++) {
                DrawItem it;
                it.text   = pieces[k].text;
                it.x      = x;
                it.y      = y;
                it.px     = px;
                it.fontId = pieces[k].font;
                it.color  = (pieces[k].color & 0x00FFFFFFu) | ((DWORD)alpha << 24);
                it.color2 = ((DWORD)alpha << 24) | 0x00000000u;
                it.shadowColor = ((DWORD)alpha << 24);   // the shadow fades with the text
                it.outlineSize = st.outline;
                it.shadowSize  = 0;
                if (pieces[k].symbol) {
                    // Same square and vertical offset the client uses for its own
                    // button sprites; the Unicode arrow stays as the fallback.
                    it.sprite      = pieces[k].symbol;
                    it.text        = ArrowFallback(pieces[k].symbol);
                    it.px          = pieces[k].width;
                    it.y           = y + (float)SymbolOffsetPx(px);
                    it.outlineSize = 0;
                }
                gts.push_back(it);
                {
                    const float m = (float)(st.outline + 2);
                    const float x0 = it.x - m, y0 = it.y - m;
                    const float x1 = it.x + (float)pieces[k].width * it.scaleX + m;
                    const float y1 = it.y + (float)lineH + m;
                    if (!gtRectValid) {
                        gtRect = RECT{ (LONG)x0, (LONG)y0, (LONG)x1, (LONG)y1 };
                        gtRectValid = true;
                    } else {
                        if ((LONG)x0 < gtRect.left)   gtRect.left = (LONG)x0;
                        if ((LONG)y0 < gtRect.top)    gtRect.top = (LONG)y0;
                        if ((LONG)x1 > gtRect.right)  gtRect.right = (LONG)x1;
                        if ((LONG)y1 > gtRect.bottom) gtRect.bottom = (LONG)y1;
                    }
                }
                x += (float)pieces[k].width;
            }
            y += (float)lineH;
        }
        i++;
    }
}

// Set while Reset runs (defined below); the scoreboard detour must not draw
// with a device that is being reset.
extern std::atomic<bool> g_inReset;

// The device the overlay last drew with. The scoreboard detour runs inside the
// frame and needs it to draw the GameText layer before the panel.
static IDirect3DDevice9* g_dev = nullptr;

// The menu layer of the previous frame. The client draws its scoreboard *before* the
// menu in the frame, so the panel painter runs while this frame's menu items do not
// exist yet; keeping the last set means the menu is under the panel from the first
// frame on, at the cost of one frame of lag when a menu opens or closes.
static std::vector<DrawItem> g_panelMenus;

IDirect3DDevice9* RenderDevice()
{
    return g_dev;
}

// Draw the GameText layer now, i.e. before the scoreboard panel is drawn. Called
// from the scoreboard detour: the panel is translucent, so the text shows through
// it the way the client's own GameText does, instead of being skipped (which is
// what happens when this hook is not installed).
// The layers the client draws *before* its scoreboard come here: the GameText and the
// native menu. Both are in-frame in the client, so the panel covers them. Drawing them
// at Present would put them on top of it, and dropping them (which is right for a
// TextDraw, see below) would hide the menu, which the client keeps drawing.
// The client asks for a line of its own UI text: rect, DT_* format, colour and the
// font height taken from its CFonts object. It is drawn here and now, from inside that
// very call, because the client paints its panels inside Present - after every hook
// this mod owns - so any later pass would land under the panel.
void DrawClientTextNow(std::vector<DrawItem>& items)
{
    if (items.empty()) return;
    if (g_inReset.load() || !g_overlayReady) return;
    IDirect3DDevice9* dev = RenderDevice();
    if (!dev) return;
    if (!StateBlockCapture(dev)) return;

    SetupState(dev);
    LockTextureCache();

    // Lay each line out: the runs of a group follow one another, the pen advancing by
    // what the previous run actually rendered, and the group as a whole is aligned in
    // the box the client gave us.
    for (size_t i = 0; i < items.size(); ) {
        const int group = items[i].group;
        size_t end = i;
        float total = 0.0f;
        if (group != 0) {
            while (end < items.size() && items[end].group == group) {
                int w = 0, h = 0;
                SetAtlasWeight(items[end].weight);
    if (GetTextTexture(dev, items[end].text, items[end].px, items[end].fontId, &w, &h))
                    total += (float)w * items[end].scaleX;
                end++;
            }
            if (items[i].align && items[i].boxW > 0) {
                const float extra = (float)items[i].boxW - total;
                if (items[i].align == 1)      items[i].x += extra * 0.5f;
                else if (items[i].align == 2) items[i].x += extra;
            }
        } else {
            int w = 0, h = 0;
            const float x0 = items[i].x;
            SetAtlasWeight(items[i].weight);
    if (GetTextTexture(dev, items[i].text, items[i].px, items[i].fontId, &w, &h) &&
                items[i].align && items[i].boxW > 0) {
                const float extra = (float)items[i].boxW - (float)w * items[i].scaleX;
                if (items[i].align == 1)      items[i].x = x0 + extra * 0.5f;
                else if (items[i].align == 2) items[i].x = x0 + extra;
            }
            end = i + 1;
        }

        float penX = items[i].x;
        for (size_t k = i; k < end; k++) {
            items[k].x = penX;
            int w = 0, h = 0, logicalW = 0;
            SetAtlasWeight(items[k].weight);
    if (GetTextTexture(dev, items[k].text, items[k].px, items[k].fontId, &w, &h, &logicalW))
                penX += (float)(logicalW > 0 ? logicalW : w) * items[k].scaleX;
            else
                penX += (float)items[k].px * (float)items[k].text.size() * items[k].scaleX;
        }
        i = end;
    }

    for (size_t i = 0; i < items.size(); i++) DrawOneItem(dev, items[i], false);
    for (size_t i = 0; i < items.size(); i++) DrawOneItem(dev, items[i], true);

    UnlockTextureCache();
    StateBlockApply();
}

void DrawUnderPanel(IDirect3DDevice9* dev)
{
    if (!dev || g_inReset.load()) return;
    if (IsGamePaused()) return;      // the client draws nothing while paused

    int screenW = *(int*)samp::kAddrScreenW;
    int screenH = *(int*)samp::kAddrScreenH;
    g_stretchW = screenW > 0 ? screenW : 640;
    g_stretchH = screenH > 0 ? screenH : 480;

    std::vector<DrawItem> gts, menus;
    RECT gtRect = { 0, 0, 0, 0 };
    bool gtRectValid = false;

    ListLock();
    BuildGameTextLayer(gts, gtRect, gtRectValid, ::GetTickCount(), screenW, screenH, true);
    menus = g_panelMenus;
    ListUnlock();

    if (gts.empty() && menus.empty()) return;
    if (g_cfg.verbose) {
        static DWORD lastUnder = 0;
        const DWORD n = ::GetTickCount();
        if (n - lastUnder >= 1000) {
            lastUnder = n;
            LogDbg(L"under panel: gtPieces=%d menuPieces=%d", (int)gts.size(), (int)menus.size());
        }
    }
    if (!StateBlockCapture(dev)) return;
    SetupState(dev);
    LockTextureCache();
    // The client's order: GameText first, the menu over it, the panel over both.
    for (size_t i = 0; i < gts.size(); i++)   DrawOneItem(dev, gts[i], false);
    for (size_t i = 0; i < menus.size(); i++) DrawOneItem(dev, menus[i], false);
    for (size_t i = 0; i < gts.size(); i++)   DrawOneItem(dev, gts[i], true);
    for (size_t i = 0; i < menus.size(); i++) DrawOneItem(dev, menus[i], true);
    UnlockTextureCache();
    StateBlockApply();
}

// Defined next to the vtable hooks: false when the overlay draws at EndScene, where
// the background store cannot be trusted.
bool OverlayUsesBackgroundStore();

void RenderOnOverlay(IDirect3DDevice9* dev)
{
    RetryHookFontVtable();
    RetryHookWrapperVtable();
    LogFontTextSummary();

    if (!dev) return;

    g_dev = dev;
    SetTextureDevice(dev);       // clears the cache when the device object changed

    // Back to the client's globals: laying text out against the render target
    // doubled the coordinates on a scaled window (2560-wide target in a 1280
    // window) and put a ghost copy of the menu title in the corner.
    int screenW = *(int*)samp::kAddrScreenW;
    int screenH = *(int*)samp::kAddrScreenH;
    g_stretchW = screenW > 0 ? screenW : 640;
    g_stretchH = screenH > 0 ? screenH : 480;

    DWORD now   = ::GetTickCount();
    DWORD delta = g_lastFrameTick ? (now - g_lastFrameTick) : 0;
    g_lastFrameTick = now;

    // Freeze every GameText timer by the time that did not really pass: while the
    // game is paused (the client freezes its own GameText timer there and resumes on
    // ESC), and across a long stall (alt-tab, device loss, a pause during which
    // Present was not called at all).
    //
    // Two details matter here, both were bugs:
    //  - the two cases must not both add the same delta (a pause frame with a long
    //    gap added it twice, pushing start past now);
    //  - start must never end up in the future. elapsed is unsigned, so a future
    //    start wraps to "a few billion ms" and the message never expires - which is
    //    exactly what happened after pausing right after /gt.
    //
    // Menus are not a pause: a native menu keeps the timer running, like the client.
    const bool freezeNow = IsGamePaused();
    if (freezeNow || delta > 500) {
        ListLock();
        for (size_t i = 0; i < g_gametexts.size(); i++) {
            g_gametexts[i].start += delta;
            if ((long)(g_gametexts[i].start - now) > 0) g_gametexts[i].start = now;
        }
        ListUnlock();
    }

    ListLock();
    std::vector<DrawItem> items, menus;
    for (size_t i = 0; i < g_items.size(); i++) {
        if (g_items[i].menu) menus.push_back(g_items[i]);
        else                 items.push_back(g_items[i]);
    }
    g_items.clear();

    // The client skips its whole textdraw pass while the scoreboard is open - the
    // board's own draw function is guarded by "cmp [CScoreboard], 0; jne done" in
    // front of the loop that calls the textdraw renderer - so nothing of ours may be
    // painted there either, on top of the panel or under it. The menu is a different
    // layer: the client keeps drawing it, so it is painted below the panel by the
    // scoreboard detour and skipped here.
    g_panelMenus = menus;                  // for the panel painter, see below
    if (ScoreboardUp()) {
        items.clear();
        menus.clear();
    }

    std::vector<DrawItem> gts;
    RECT gtRect = { 0, 0, 0, 0 };   // union of the GameText quads, for the snapshot
    // A capture is a read-back, so the store only covers a GameText box, never the screen.
    const LONG kBgMaxArea = 640 * 240;
    bool gtRectValid = false;
    BuildGameTextLayer(gts, gtRect, gtRectValid, now, screenW, screenH, false);

    // TextDraw pieces share the same background copy: a textdraw that disappears
    // while the client is presenting a frozen frame needs the same erase as a
    // GameText, and both go through the one stored rectangle. The width is an
    // estimate (the texture is not needed here), rounded up on purpose.
    for (size_t k = 0; k < items.size(); k++) {
        const float wpx = (float)items[k].px * (float)items[k].text.size() + (float)items[k].px;
        const float x0 = items[k].x - 4.0f, y0 = items[k].y - 4.0f;
        const float x1 = items[k].x + wpx + 4.0f, y1 = items[k].y + (float)items[k].px * 2.0f + 4.0f;
        if (!gtRectValid) {
            gtRect = RECT{ (LONG)x0, (LONG)y0, (LONG)x1, (LONG)y1 };
            gtRectValid = true;
        } else {
            if ((LONG)x0 < gtRect.left)   gtRect.left = (LONG)x0;
            if ((LONG)y0 < gtRect.top)    gtRect.top = (LONG)y0;
            if ((LONG)x1 > gtRect.right)  gtRect.right = (LONG)x1;
            if ((LONG)y1 > gtRect.bottom) gtRect.bottom = (LONG)y1;
        }
    }


    // Nothing to draw. That is not a reason to return while a background copy is
    // still held: the text may have disappeared on a frozen frame, and the restore
    // below is what removes it. The menu counts as something to draw - it is its own
    // layer, and a menu on its own (no TextDraw, no GameText) is the normal case.
    if (gts.empty() && items.empty() && menus.empty() && !g_bgValid) return;



    if (g_cfg.verbose) {
        static DWORD lastSum = 0;
        const DWORD n = ::GetTickCount();
        if (n - lastSum >= 1000) {
            lastSum = n;
            // Geometry as well: a line that is missing on screen but present here
            // means it was drawn off-screen (px/lineH from a stale screen size)
            // rather than skipped.
            float yFirst = -1.0f, yLast = -1.0f;
            for (size_t i = 0; i < gts.size(); i++) {
                if (yFirst < 0.0f) yFirst = gts[i].y;
                yLast = gts[i].y;
            }
            // elapsed/time of the first message: a timer that stops moving while a menu,
            // the pause screen or another overlay holds the frame shows up here.
            DWORD gtAge = 0, gtTotal = 0;
            {
                ListLock();
                if (!g_gametexts.empty()) {
                    gtAge   = n - g_gametexts[0].start;
                    gtTotal = (DWORD)g_gametexts[0].timeMs;
                }
                ListUnlock();
            }
            LogDbg(L"overlay: screen=%dx%d gtItems=%d gtPieces=%d tdPieces=%d menuPieces=%d px=%d yFirst=%.0f yLast=%.0f paused=%d gt0=%u/%u",
                   screenW, screenH, (int)g_gametexts.size(), (int)gts.size(), (int)items.size(),
                   (int)menus.size(),
                   gts.empty() ? 0 : gts[0].px, yFirst, yLast, (int)IsGamePaused(), gtAge, gtTotal);
            // Which strings come through the TextDraw path: more than one draw of the
            // same GameText text means the client renders it twice.
            for (size_t i = 0; i < items.size() && i < 2; i++) {
                LogDbg(L"td[%d]: px=%d at (%.0f,%.0f) '%s'", (int)i, items[i].px, items[i].x, items[i].y,
                       items[i].text.c_str());
            }
        }
    }

    // The client stops drawing its own text while the pause menu holds the game, so
    // the overlay hides too. Restoring the background first is what makes this safe:
    // without it, hiding would leave the last drawn pixels on a frozen frame.
    const bool pausedNow = IsGamePaused();

    if (!StateBlockCapture(dev)) return;
    SetupState(dev);

    if (pausedNow) {
        if (g_bgValid) {
            BgErase(dev);
            g_bgValid = false;
        }
        StateBlockApply();
        return;
    }

    // Background bookkeeping, after the state is ours so the erase can draw:
    //   - the message is gone: paint the stored background over it, then forget it;
    //   - the rectangle changed (new message, moved text): paint the old one back and
    //     capture the new area before drawing on it;
    //   - unchanged: nothing to do, the text covers its own pixels anyway.
    //
    // Erasing is only correct on a frozen frame: the stored background is at least
    // one frame old, so pasting it over a live world shows a patch of the previous
    // frame - the flash seen when a message is replaced right after its fade. A live
    // frame repaints the area anyway, so skipping the erase there is both cheaper and
    // the only correct choice.
    const bool menuThisFrame = MenuDrawnThisFrame();
    const bool frozenNow = IsGamePaused() || menuThisFrame || g_menuFramePrev;
    g_menuFramePrev = menuThisFrame;
    // The store exists to put back what a fading GameText covered. A capture is a GPU->CPU
    // read-back (GetRenderTargetData), which stalls the whole pipeline, so it has to stay small:
    // a 2414x1050 rectangle per frame dropped this game to about one frame a second, and erasing
    // it pasted a stale (night-black) frame over the world. Only the GameText's own box is ever
    // worth copying, and only while there is a GameText to protect.
    const LONG bgArea = (gtRect.right - gtRect.left) * (gtRect.bottom - gtRect.top);
    const bool bgWanted = OverlayUsesBackgroundStore() && gtRectValid && !gts.empty() &&
                          bgArea > 0 && bgArea <= kBgMaxArea;
    if (!bgWanted) {
        if (g_bgValid) g_bgValid = false;      // drop any copy captured under the old rules
    } else {
        const bool same = g_bgValid &&
                          g_bgRect.left == gtRect.left && g_bgRect.top == gtRect.top &&
                          g_bgRect.right == gtRect.right && g_bgRect.bottom == gtRect.bottom;
        if (!same) {
            if (g_bgValid && frozenNow) BgErase(dev);
            g_bgValid = false;
            BgCapture(dev, gtRect);
        }
    }
    if (g_cfg.verbose) {
        static DWORD lastBg = 0;
        const DWORD n2 = ::GetTickCount();
        if (n2 - lastBg >= 1000) {
            lastBg = n2;
            LogDbg(L"bg: valid=%d rect=%ld,%ld %ldx%ld held=%dx%d",
                   (int)g_bgValid, g_bgRect.left, g_bgRect.top,
                   g_bgRect.right - g_bgRect.left, g_bgRect.bottom - g_bgRect.top, g_bgW, g_bgH);
        }
    }

    // The cache lock is held for the whole draw: Reset runs on the main thread and
    // used to free the textures we were about to pass to SetTexture, which is a
    // use-after-free. The critical section is recursive, so the cache calls inside
    // DrawOneItem re-enter it safely.
    // Per-frame timing, so a slow frame can be attributed instead of guessed at.
    const DWORD tDraw0 = ::GetTickCount();
    LockTextureCache();
    for (size_t i = 0; i < gts.size(); i++)   SafeDrawItem(dev, gts[i], false);
    for (size_t i = 0; i < items.size(); i++) SafeDrawItem(dev, items[i], false);
    for (size_t i = 0; i < menus.size(); i++) SafeDrawItem(dev, menus[i], false);
    for (size_t i = 0; i < gts.size(); i++)   SafeDrawItem(dev, gts[i], true);
    for (size_t i = 0; i < items.size(); i++) SafeDrawItem(dev, items[i], true);
    for (size_t i = 0; i < menus.size(); i++) SafeDrawItem(dev, menus[i], true);
    UnlockTextureCache();
    {
        static DWORD s_last = 0, s_frames = 0, s_tex = 0, s_buildMs = 0;
        static DWORD s_lastTex = 0;
        s_frames++;
        const DWORD tDrawMs = ::GetTickCount() - tDraw0;
        s_buildMs += tDrawMs;
        const DWORD nowMs = ::GetTickCount();
        const DWORD tex = TextureBuildCount();
        if (nowMs - s_last >= 2000) {
            const float fps = (float)s_frames * 1000.0f / (float)(nowMs - s_last);
            LogW(L"frame: fps=%.1f draw=%lums/frame pieces=%d textures=%u (+%u since last)",
                 fps, s_frames ? s_buildMs / s_frames : 0,
                 (int)(gts.size() + items.size() + menus.size()), tex, tex - s_lastTex);
            s_last = nowMs; s_frames = 0; s_buildMs = 0; s_lastTex = tex;
        }
        (void)s_tex;
    }

    StateBlockApply();
}

// Set while Reset runs: the render thread must not draw with a device that is
// being reset, and must not race the cache clear that follows.
std::atomic<bool> g_inReset{ false };

void RenderOnReset()
{
    g_inReset = true;
    // A D3DSBT_ALL block restores the viewport and scissor it captured. Keeping the
    // one from before the resize puts the old viewport back after every frame of the
    // new backbuffer, which is exactly the distorted split picture a resize showed.
    DropStateBlock();
    BgDrop();         // D3DPOOL_DEFAULT surfaces do not survive a Reset
    LogW(L"device Reset, state block and texture cache dropped");
}

// ---------------------------------------------------------------------------
// vtable hooks
// ---------------------------------------------------------------------------
// Where the overlay draws. Present is the default and the place the background
// capture and the restore were written for: the swap chain's back buffer is
// unambiguous, while at EndScene the bound target is whatever the game left behind.
// EndScene is the alternative for layering: every overlay that draws at Present
// (omp-client's ImGui settings window, RTSS, other ImGui mods) is a detour that runs
// after EndScene, so drawing there lets them cover us instead of the other way round
// ([overlay] hook_point).
static __declspec(thread) int t_drawDepth = 0;

// Optional EndScene hook. Everything that draws at Present - omp-client's ImGui
// settings window, RTSS, another ImGui mod - is a detour that runs *before* us in
// the same chain, so drawing at Present always puts us on top of them. EndScene
// runs before any of those detours, so with hook_point = endscene the overlay goes
// underneath them instead. Present stays the default: at EndScene the bound render
// target is whatever the game left behind, while Present always has the back buffer
// that is about to be shown.
typedef HRESULT(__stdcall* EndScene_t)(IDirect3DDevice9*);
static EndScene_t g_origEndScene = nullptr;
static void*      g_prevEndScene = nullptr;

// At Present the bound render target is the back buffer by definition. At EndScene
// it is not: the game renders into textures first (the camera feed behind the radar,
// mirrors), and drawing there puts the overlay on an off-screen surface. Worse, the
// background copy would then hold the wrong picture, and a frozen frame gets that
// picture pasted over the text instead of the world - which looks exactly like a
// GameText that never disappears. Only the EndScene of the visible frame draws.
static bool OnBackBuffer(IDirect3DDevice9* dev)
{
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return false;

    bool same = false;
    IDirect3DSwapChain9* chain = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &chain)) && chain) {
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            same = (rt == bb);
            bb->Release();
        }
        chain->Release();
    }
    rt->Release();
    return same;
}

// Set while the overlay draws at EndScene. The background store (capture before the
// draw, paste back when a frozen frame would otherwise keep the text) assumes the
// Present rules: the target is the back buffer of the frame that is about to be
// shown. At EndScene that is not guaranteed and the paste-back is exactly what made
// a message stick to the screen while a menu, the pause screen or another overlay
// held the frame, so it is switched off there.
static __declspec(thread) bool t_atEndScene = false;

bool OverlayUsesBackgroundStore()
{
    return !t_atEndScene;
}

static void DrawOverlayGuarded(IDirect3DDevice9* dev)
{
    if (!dev || dev != g_device || g_inReset || !g_overlayReady) return;
    if (t_drawDepth != 0) return;             // already inside our own call
    if (g_cfg.hookEndScene && !OnBackBuffer(dev)) {
        static bool once = false;
        if (!once) {
            once = true;
            LogW(L"endscene: render target is not the back buffer, off-screen passes skipped");
        }
        return;
    }
    t_drawDepth++;
    t_atEndScene = g_cfg.hookEndScene;
    __try {
        RenderOnOverlay(dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        StateBlockApply();
        static bool once = false;
        if (!once) { once = true; LogW(L"exception while drawing, frames skipped"); }
    }
    t_atEndScene = false;
    t_drawDepth--;
}

static HRESULT __stdcall EndSceneDetour(IDirect3DDevice9* dev)
{
    if (g_cfg.hookEndScene) DrawOverlayGuarded(dev);
    if (!g_origEndScene) {
        static bool once = false;
        if (!once) { once = true; LogW(L"EndScene has no original, the call is swallowed"); }
        return D3D_OK;
    }
    return g_origEndScene(dev);
}

// The pause menu is drawn inside the frame, so restoring the background at Present
// painted a patch of the world on top of it for one frame - the flash on ESC. Doing
// it right after BeginScene means the menu is drawn over the restored pixels.
static HRESULT __stdcall BeginSceneDetour(IDirect3DDevice9* dev)
{
    const HRESULT hr = g_origBeginScene ? g_origBeginScene(dev) : D3D_OK;
    if (SUCCEEDED(hr) && g_origBeginScene && dev && dev == g_device && g_overlayReady &&
        !g_inReset && g_bgValid && (IsGamePaused() || g_menuFramePrev)) {
        if (StateBlockCapture(dev)) {
            SetupState(dev);
            BgErase(dev);
            StateBlockApply();
        }
        g_bgValid = false;
    }
    return hr;
}


static HRESULT __stdcall PresentDetour(IDirect3DDevice9* dev, const RECT* srcRect,
                                       const RECT* dstRect, HWND hwnd, const RGNDATA* dirty)
{
    if (!g_origPresent) return D3D_OK;

    // Chain guard: another overlay may re-dispatch through the vtable and land here
    // from inside our own call (DrawOverlayGuarded keeps that counter, MSVC forbids
    // __try in a function that needs C++ unwinding, so there is no scope guard).

    // hook_point = present (the default): draw here, i.e. on top of everything.
    // hook_point = endscene: EndSceneDetour already drew, so do nothing here.
    if (!g_cfg.hookEndScene) DrawOverlayGuarded(dev);

    return g_origPresent(dev, srcRect, dstRect, hwnd, dirty);
}
static HRESULT __stdcall ResetDetour(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    // Guard against calling ourselves: if g_origReset ever ends up pointing at this
    // function (a re-hook race, see RenderHookDevice), the call below recurses
    // until the stack dies. The log showed thousands of entries in one tick.
    static thread_local bool inReset = false;
    if (inReset) {
        static bool once = false;
        if (!once) { once = true; LogW(L"Reset re-entered, chaining skipped"); }
        return D3D_OK;
    }
    inReset = true;

    RenderOnReset();
    if (pp) LogW(L"Reset: backbuffer %ux%u, windowed=%d", pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->Windowed);
    HRESULT hr = g_origReset ? g_origReset(dev, pp) : D3D_OK;
    // Clear after the call: the device is valid again, and nothing can be drawing
    // because the frame hook was skipped while g_inReset was set.
    ClearTextureCache();
    g_inReset = false;
    inReset = false;
    return hr;
}

// Does this address belong to a loaded module? Heap thunks (what an overlay
// installs into a device vtable) do not, the real d3d9 entry point does.
static bool IsModuleAddress(void* addr)
{
    if (!addr) return false;

    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)addr, &mod)) {
        return false;
    }
    return mod != nullptr;
}
// Write one pointer into a vtable slot, temporarily lifting page protection.
static bool PatchSlot(void** slot, void* value)
{
    DWORD old = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *slot = value;
    ::VirtualProtect(slot, sizeof(void*), old, &old);
    ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
    return true;
}
bool RenderHookDevice(IDirect3DDevice9* dev)
{
    if (!dev) return false;

    // Called from the init thread and from the watchdog; without this two threads
    // can both see an unhooked slot and both patch it.
    static std::mutex hookMutex;
    std::lock_guard<std::mutex> guard(hookMutex);

    if (dev == g_device && g_origPresent && g_origReset) return true;   // already ours

    void** vt = *(void***)dev;
    if (!vt) return false;

    // A vtable living in heap memory means the object is a proxy installed by
    // another mod, not the real d3d9 device. Hooking it breaks that mod's own
    // chaining (the frame then never ends: frozen picture, sound still playing),
    // so leave those alone.
    if (!IsModuleAddress((void*)vt)) {
        LogW(L"device %p uses a heap vtable (%p) - proxy object, leaving it alone", (void*)dev, (void*)vt);
        g_device = dev;      // remember it so the watchdog stops retrying
        return false;
    }

    // Never hook twice: if a slot already points at our detour, storing it as
    // "original" would call ourselves recursively.
    if (vt[17] == (void*)&PresentDetour || vt[41] == (void*)&BeginSceneDetour ||
        vt[42] == (void*)&EndSceneDetour || vt[16] == (void*)&ResetDetour) {
        g_device = dev;
        LogW(L"device vtable already hooked, skipping");
        return true;
    }

    void* prevPresent    = vt[17];         // IDirect3DDevice9::Present
    void* prevBeginScene = vt[41];         // IDirect3DDevice9::BeginScene
    void* prevEndScene   = vt[42];         // IDirect3DDevice9::EndScene
    void* prevReset      = vt[16];         // IDirect3DDevice9::Reset

    // Another thread may have patched the slot between the check above and this
    // line. Storing our own detour as "original" makes the detour call itself.
    if (prevPresent == (void*)&PresentDetour || prevBeginScene == (void*)&BeginSceneDetour ||
        prevEndScene == (void*)&EndSceneDetour || prevReset == (void*)&ResetDetour) {
        LogW(L"device vtable changed while hooking, keeping the previous originals");
        g_device = dev;
        return true;
    }

    // Fill in the originals first: the render thread may already be inside our
    // detour the instant the slot changes.
    g_origPresent    = (Present_t)prevPresent;
    g_origBeginScene = (BeginScene_t)prevBeginScene;
    g_origEndScene   = (EndScene_t)prevEndScene;
    g_origReset      = (Reset_t)prevReset;
    g_device         = dev;
    g_vtSlots        = vt;
    g_prevPresent    = prevPresent;
    g_prevBeginScene = prevBeginScene;
    g_prevEndScene   = prevEndScene;
    g_prevReset      = prevReset;

    if (!PatchSlot(&vt[17], (void*)&PresentDetour)) {
        LogW(L"could not patch Present slot (VirtualProtect failed)");
        return false;
    }
    if (!PatchSlot(&vt[41], (void*)&BeginSceneDetour)) {
        LogW(L"could not patch BeginScene slot (VirtualProtect failed)");
        PatchSlot(&vt[17], prevPresent);
        return false;
    }
    if (!PatchSlot(&vt[42], (void*)&EndSceneDetour)) {
        LogW(L"could not patch EndScene slot (VirtualProtect failed)");
        PatchSlot(&vt[17], prevPresent);
        PatchSlot(&vt[41], prevBeginScene);
        return false;
    }
    if (!PatchSlot(&vt[16], (void*)&ResetDetour)) {
        LogW(L"could not patch Reset slot (VirtualProtect failed)");
        PatchSlot(&vt[17], prevPresent);
        PatchSlot(&vt[41], prevBeginScene);
        PatchSlot(&vt[42], prevEndScene);
        return false;
    }

    g_overlayReady = true;
    LogW(L"hooked: Present=%p, BeginScene=%p, Reset=%p", prevPresent, prevBeginScene, prevReset);
    return true;
}

// Put the device slots back. Called from UnhookAll: leaving them pointed at our
// detour would send the next Present or EndScene into unmapped memory after
// FreeLibrary.
void RenderUnhookDevice()
{
    if (g_vtSlots) {
        if (g_prevPresent)    PatchSlot(&g_vtSlots[17], g_prevPresent);
        if (g_prevBeginScene) PatchSlot(&g_vtSlots[41], g_prevBeginScene);
        if (g_prevEndScene)   PatchSlot(&g_vtSlots[42], g_prevEndScene);
        if (g_prevReset)      PatchSlot(&g_vtSlots[16], g_prevReset);
        g_vtSlots = nullptr;
    }
    g_prevPresent    = nullptr;
    g_prevBeginScene = nullptr;
    g_prevEndScene   = nullptr;
    g_prevReset      = nullptr;
    g_origEndScene   = nullptr;
    g_overlayReady = false;
    // Resources would outlive the hook otherwise: the state block, the
    // background surfaces (D3DPOOL_DEFAULT) and every cached texture stay
    // allocated after unload.
    DropStateBlock();
    BgDrop();
    ClearTextureCache();

    LogW(L"device vtable restored");
}

void RenderInit()
{
    ListLock();
    ListUnlock();
}




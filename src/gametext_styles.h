// ---------------------------------------------------------------------------
// gametext_styles.h - the built-in GameText layout table
//
// Shared by render.cpp (the real overlay) and the offline preview harness, so a
// preview can never drift from what the game shows.
//
// These are not the numbers the stock client enforces. The client hard-codes
// three quirks - exactly the ones the open.mp wiki marks as "fixed in fixes.inc /
// GameText+": style 0 ignores the requested time and wipes every other on-screen
// text for 9 s, style 1 fades out after 8 s and loops, style 5 is pinned to 3 s
// and refuses "spammed" messages. fixes.inc (FIX_GameText) and GameText+ work
// around that by drawing the styles with textdraws and hiding them with a timer,
// so a message stays for exactly the time the server asked for.
//
// The values are GameText+' gs_StyleList, which fixes.inc matches: anchor on the
// 640x448 screen, text height in TextDrawLetterSize units, default colour,
// alignment, outline and the TextDrawFont face.
// ---------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include <d3d9types.h>   // D3DCOLOR

struct GtStyle {
    float    x, y;       // anchor on the 640x448 virtual screen (textdraw units)
    float    letterH;    // text height, same units as a TextDraw letter height
    D3DCOLOR color;      // default colour of the style (AARRGGBB)
    int      align;      // 0 = left, 1 = centre, 2 = right
    int      outline;    // outline thickness in pixels
    int      font;       // TextDrawFont id the fix uses for this style
};

static const GtStyle kGtStyles[] = {
    //  x       y       letterH  colour        align  outline  font
    { 320.0f, 214.0f, 3.60f, 0xFF906210u, 1, 2, 3 },   // 0 mission passed (gold, Pricedown)
    { 620.0f, 310.0f, 2.60f, 0xFF906210u, 2, 2, 3 },   // 1 mission name, bottom right
    { 320.0f, 156.0f, 4.20f, 0xFFE1E1E1u, 1, 3, 0 },   // 2 big "wasted" style (blackletter)
    { 320.0f, 154.5f, 2.75f, 0xFF906210u, 1, 2, 2 },   // 3 wide capitals (Bank Gothic)
    { 320.0f, 115.5f, 2.75f, 0xFF906210u, 1, 2, 2 },   // 4
    { 320.0f, 217.0f, 2.75f, 0xFFE1E1E1u, 1, 2, 2 },   // 5
    { 320.0f,  60.0f, 3.60f, 0xFFACCBF1u, 1, 2, 3 },   // 6
};
static const int kGtStyleCount = (int)(sizeof(kGtStyles) / sizeof(kGtStyles[0]));

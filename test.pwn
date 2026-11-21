// samp-unicode manual test filterscript
//
//   compile : pawncc test.pwn -o test.amx
//   install : copy test.amx to filterscripts\, add `test` to server.cfg
//
// Every command hides the previous set first, so one screen tests one thing.
//
//   --- text ---
//   /td    fonts 0..3, one line each: blackletter / sans / wide caps / Pricedown
//   /tds   letterSize sweep: same text at 0.6 / 1.2 / 1.8 / 2.4 height
//   /tdf   flag matrix: no shadow, no outline, both, boxed with explicit size
//   /tdc   colour codes: ~b~ ~g~ ~h~ ~l~ ~p~ ~r~ ~s~ ~w~, {RRGGBB}, {AARRGGBB},
//          and an unknown ~x~ that must stay literal
//   /tdn   line breaks: ~n~ with centre / left alignment
//   /tdw   letterWidth sweep: 0.15 / 0.35 / 0.70 with the same height
//   /tdx   edge cases: empty string, spaces only, colour codes only, one glyph
//   /tdl   very long line (tests texture truncation at 4096 px)
//
//   --- other renderers ---
//   /gt N [ms]  GameText style N (0..6) for ms (default 5000): the fixed layout,
//          the style's own colour, and exactly the requested duration
//   /gtfix style 0 probe: a TextDraw must stay visible while it runs, and the
//          text must go after the time asked for (5 s), not after the client's
//          9 s, and style 0 must not blank anything
//   /spr   font 4 (txd sprite) and font 5 (3D model preview) - must stay untouched
//   /ptd   player TextDraws, fonts 1..3 - check the log to see if they are hooked
//   /3d    3D text label above you - never hooked
//   /move  animate a position (text unchanged, texture must be reused)
//   /num   animate the text (every tick is a cache miss)
//   /menu  native GTA menu (title + items in Chinese) - shows whether the
//          client renders menus at all with a real font, or still boxes
//   /menuoff hide it
//   /tdoff hide everything          /hide  hide + stop timers + remove label
//   /tdb /tdj /tdk /tdr /tdu /tdm   the same four-font page in Big5 / Shift-JIS /
//                                   EUC-KR / CP1251 / UTF-8 / UTF-8 mixed (/td = GBK)
//   /tdcode /tdcodes   colour codes and ~k~ key names, one line per code
//   /tdlight           ~h~ lightening per colour, plus ~n~ across colours
//   /tdstars /tdglyphs ~]~ and the client's own arrow glyphs on black
//   /tdbox             native box next to the box this mod draws
//   /os <o> <s>        outline/shadow sweep: /os 1 1, /os 3 2, /os 0 0
//   /box /box2 /box3   box geometry probes
//   /lang /langgt      five languages on one page, and in GameText
//   /chat /spam [ms]   chat stays the client's own text path (never hooked)
//   /gta N             pure-ASCII GameText style N: the client's own arrows
//   /sel /selshow /selhide   selectable TextDraws and selection mode
//   /ascii /gtascii N        punctuation probe: TextDraw faces / GameText style
//
// Every non-ASCII byte is written as \xNN, so this file stays pure ASCII and the
// exact bytes reach the client whatever encoding the compiler assumes.

#define FILTERSCRIPT
#include <a_samp>

// The characters whose client glyph is worth checking: brackets, braces, pipe,
// backslash, caret, underscore, backtick and the common punctuation, spaced so
// each one can be read off a screenshot. No % (format strings) on purpose.
// (no ~ at all: it is the code delimiter, an unmatched one makes the client show
// "Error: unmatched tilde" instead of the string)
#define PUNCT "[ ] { } | \\ ^ _ ` @ # $ & * ( ) < > ? / : ; , . = + - A Z a z 0 9"

// "hello, world! welcome to the server"
#define L_GBK   "\xC4\xE3\xBA\xC3\xA3\xAC\xCA\xC0\xBD\xE7\xA3\xA1\xBB\xB6\xD3\xAD\xC0\xB4\xB5\xBD\xB7\xFE\xCE\xF1\xC6\xF7"
#define L_BIG5  "\xA7\x41\xA6n\xA1\x41\xA5@\xAC\xC9\xA1I\xC5w\xAA\xEF\xA8\xD3\xA8\xEC\xA6\xF8\xAA\x41\xBE\xB9"
#define L_SJIS  "\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD\x81\x41\x90\xA2\x8A\x45\x81I\x83T\x81[\x83o\x81[\x82\xD6\x82\xE6\x82\xA4\x82\xB1\x82\xBB"
#define L_EUCKR "\xBE\xC8\xB3\xE7\xC7\xCF\xBC\xBC\xBF\xE4, \xBC\xBC\xB0\xE8! \xBC\xAD\xB9\xF6\xBF\xA1 \xBF\xC0\xBD\xC5 \xB0\xCD\xC0\xBB \xC8\xAF\xBF\xB5\xC7\xD5\xB4\xCF\xB4\xD9"
#define L_1251  "\xCF\xF0\xE8\xE2\xE5\xF2, \xEC\xE8\xF0! \xC4\xEE\xE1\xF0\xEE \xEF\xEE\xE6\xE0\xEB\xEE\xE2\xE0\xF2\xFC \xED\xE0 \xF1\xE5\xF0\xE2\xE5\xF0"

// the same sentence in UTF-8, and a mixed-script line that only UTF-8 can carry
#define L_UTF8  "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x81\xE6\xAC\xA2\xE8\xBF\x8E\xE6\x9D\xA5\xE5\x88\xB0\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8"
#define L_MIX   "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87 \xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4 \xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"

// short "test" marker per encoding, so every probe line contains non-ASCII and
// therefore actually reaches the plugin instead of being passed through
#define K_GBK   "\xB2\xE2\xCA\xD4"
#define K_BIG5  "\xB4\xFA\xB8\xD5"
#define K_SJIS  "\x83\x65\x83\x58\x83\x67"
#define K_EUCKR "\xBD\xC3\xC7\xE8"
#define K_1251  "\xD2\xE5\xF1\xF2"
#define K_UTF8  "\xE6\xB5\x8B\xE8\xAF\x95"

new const gLocale[7][] = {
    "GBK", "Big5", "Shift-JIS", "EUC-KR", "CP1251", "UTF-8", "UTF-8 mixed"
};

new const gLocal[7][] = {
    L_GBK, L_BIG5, L_SJIS, L_EUCKR, L_1251, L_UTF8, L_MIX
};

new const gMark[7][] = { K_GBK, K_BIG5, K_SJIS, K_EUCKR, K_1251, K_UTF8, K_UTF8 };

new gLang = 0;

new Text:gTd[4];          // fonts 0..3
new Text:gSize[4];        // letterSize sweep
new Text:gFlags[4];       // shadow / outline / box matrix
new Text:gTdo;            // outline / shadow sweep (/os)
new Text:gBoxTd;          // real box probe (/box)

// random chat flood (/spam): varied content at a fixed interval, one timer per
// player, chained so it stops as soon as the flag clears.
new gSpamOn[MAX_PLAYERS];
new gSpamMs[MAX_PLAYERS];
new Text:gColor;          // colour codes
new Text:gLangTd;         // /lang: five-line pangram, right side
new Text:gAlpha[4];       // /tda: the same line at four alphas
new Text:gCodes;          // /tdcodes: one line per ~x~ colour code
new Text:gKey;            // /tdcodes: ~k~ needs a short string, the client refuses k-codes in long ones
new Text:gGlyphs;         // /tdglyphs: the client's own arrow/star glyphs on black, for mask capture
new Text:gStarBg;         // /tdstars: black backdrop for the star hunt
new Text:gStars[4];       // /tdstars: ~]~ in font 0..3
new Text:gStarTd;        // /tdstars: A~]~B on black, for the star mask
new Text:gLight;          // /tdlight: the ~h~ ladder per colour, plus A/B/C across ~n~
new Text:gBreak[2];      // ~n~ probes: centre and left alignment
new Text:gWide[3];        // letterWidth sweep
new Text:gEdge[5];        // empty / spaces / codes only / one glyph / ASCII control
new Text:gLong;           // over-long line
new Text:gBox[2];         // box probe: native (ASCII) vs ours (marker)
new Text:gGtKeep;         // stays on screen during the style 0 probe
new Text:gSpr[5];         // font 4 candidates and font 5
new Text:gMove;           // animated position
new Text:gNum;            // animated text
new Text:gSel[3];         // /sel: selectable textdraws (CJK, ASCII, CJK in a box)
new Text:gAscii[4];       // /ascii: the punctuation probe, one line per font
new bool:gSelMode[MAX_PLAYERS];

new PlayerText:gPtd[3];
new Text3D:gLabel;

new Menu:gMenu;                 // native menu probe (/menu), 1 column
new Menu:gMenu2;                // 2-column probe (/menu2)
new Menu:gLangMenu;              // /lang: hello in several languages (native menu, left)

new bool:gPtdCreated;
new bool:gLabelCreated;
new bool:gMoveOn;
new bool:gNumOn;
new gMoveX = 100;
new gNumTick;
new gMoveTimer;
new gNumTimer;

forward MoveTick();
forward SendNote(playerid, const note[]);
forward NumTick();

// Default letter size follows the open.mp note that fonts look best at an X:Y ratio
// of 1:4 (TextDrawLetterSize: "if x is 0.5 then y should be 2"). 0.35 x 1.40 is the
// comfortable body-text size; the sweeps below override it on purpose.
static Text:Make(Float:x, Float:y, font, const text[], Float:w = 0.35, Float:h = 1.4,
                 shadow = 1, outline = 1)
{
    new Text:t = TextDrawCreate(x, y, text);
    TextDrawFont(t, font);
    TextDrawLetterSize(t, w, h);
    TextDrawColor(t, 0xFFFFFFFF);
    TextDrawSetShadow(t, shadow);
    TextDrawSetOutline(t, outline);
    TextDrawBackgroundColor(t, 0x000000AA);
    return t;
}

public OnFilterScriptInit()
{
    // one line per font, different alignment, the last one boxed
    gTd[0] = Make(20.0,  40.0, 0, " ");
    TextDrawAlignment(gTd[0], 0);
    gTd[1] = Make(320.0, 65.0, 1, " ");
    TextDrawAlignment(gTd[1], 1);
    gTd[2] = Make(620.0, 90.0, 2, " ");
    TextDrawAlignment(gTd[2], 2);
    gTd[3] = Make(20.0, 115.0, 3, " ");
    TextDrawAlignment(gTd[3], 0);
    TextDrawUseBox(gTd[3], 1);
    TextDrawBoxColor(gTd[3], 0x000000AA);

    // same text, growing letter height (0.6 .. 2.4)
    for (new i = 0; i < 4; i++) {
        gSize[i] = Make(20.0, 190.0 + 45.0 * i, 1, " ", 0.35, 0.6 + 0.6 * i);
    }

    // shadow / outline / box matrix. Transparent background and a larger size on
    // purpose: a translucent backdrop plus 1px effects at 20px text hides the very
    // difference this probe is supposed to show.
    gFlags[0] = Make(20.0, 290.0, 1, " ", 0.4, 1.6, 0, 2);   // outline 2px
    gFlags[1] = Make(20.0, 315.0, 1, " ", 0.4, 1.6, 2, 0);   // shadow 2px
    gFlags[2] = Make(20.0, 340.0, 1, " ", 0.4, 1.6, 1, 1);   // both
    gFlags[3] = Make(20.0, 365.0, 1, " ", 0.4, 1.6, 1, 1);   // boxed, fixed size

    for (new i = 0; i < 4; i++) {
        TextDrawBackgroundColor(gFlags[i], 0x00000000);     // no backdrop
        TextDrawColor(gFlags[i], 0xFFFFFFFF);               // white text
    }

    TextDrawUseBox(gFlags[3], 1);
    TextDrawTextSize(gFlags[3], 320.0, 385.0);   // bottom-right corner, not a size
    TextDrawBoxColor(gFlags[3], 0x00000088);      // RRGGBBAA: black, alpha 0x88

    gColor = Make(20.0, 400.0, 1, " ", 0.3, 1.0);
    // Five languages at 0.22 x 1.32 (1:6) stack from y=60 down to about y=223 with nothing clipped,
    // and alignment 2 centres every line on x=320.
    gLangTd = Make(320.0, 60.0, 1, " ", 0.22, 1.32);
    TextDrawAlignment(gLangTd, 2);            // 2 = centred on x, so the five lines stack down the middle
    TextDrawBackgroundColor(gLangTd, 0x000000C0);
    gCodes = Make(20.0, 20.0, 1, " ", 0.45, 1.55);
    gKey   = Make(20.0, 390.0, 1, "~k~~PED_FIREWEAPON~", 0.55, 1.85);

    // One glyph per line, white on opaque black, so the capture thresholds cleanly.
    gGlyphs = Make(30.0, 30.0, 1, "~u~~n~~d~~n~~<~~n~~>~~n~~]~", 0.45, 1.55, 0, 0);
    TextDrawUseBox(gGlyphs, 1);
    TextDrawBoxColor(gGlyphs, 0x000000FF);
    TextDrawTextSize(gGlyphs, 600.0, 150.0);   // wide enough for the mixed arrow line, no taller than the five rows

    // The star did not show up in font 1, so hunt for it: same black backdrop, one
    // textdraw per font, each with the code and a letter after it.
    gStarBg = Make(20.0, 20.0, 1, " ", 0.45, 1.55, 0, 0);
    TextDrawUseBox(gStarBg, 1);
    TextDrawBoxColor(gStarBg, 0x000000FF);
    TextDrawTextSize(gStarBg, 420.0, 260.0);
    for (new i = 0; i < 4; i++)
    {
        gStars[i] = Make(40.0, 40.0 + 55.0 * i, i, "A~]~B", 0.45, 1.55, 0, 0);
    }

    gStarTd = Make(30.0, 30.0, 1, "A~]~B", 0.45, 1.55, 0, 0);
    TextDrawUseBox(gStarTd, 1);
    TextDrawBoxColor(gStarTd, 0x000000FF);
    TextDrawTextSize(gStarTd, 260.0, 90.0);

    // /tdlight: one line per colour code showing its ~h~ ladder from plain to as
    // white as it goes, on black so the fill colour can be measured directly. The
    // last line is the newline probe: the client replays the last colour code at the
    // start of the following line, so B and C must come out one and two steps
    // lighter than A, not the same colour.
    gLight = Make(30.0, 30.0, 1, " ", 0.45, 1.55, 0, 0);
    TextDrawUseBox(gLight, 1);
    TextDrawBoxColor(gLight, 0x000000FF);
    TextDrawTextSize(gLight, 340.0, 300.0);

    TextDrawSetOutline(gCodes, 0);        // no outline: the fill colour must be measurable
    TextDrawSetShadow(gCodes, 0);

    // ~n~ with centre and left alignment
    gBreak[0] = Make(320.0, 60.0, 1, " ", 0.35, 1.3);
    TextDrawAlignment(gBreak[0], 1);
    gBreak[1] = Make(20.0, 130.0, 1, " ", 0.35, 1.3);
    TextDrawAlignment(gBreak[1], 0);

    // letterWidth sweep, same height
    for (new i = 0; i < 3; i++) {
        gWide[i] = Make(20.0, 250.0 + 30.0 * i, 1, " ",
                         (i == 0) ? 0.15 : ((i == 1) ? 0.35 : 0.70), 1.2);
    }

    // edge cases. Each row carries its own box so the four rows stay
    // identifiable when their text is empty: black = empty string,
    // blue = spaces only, green = colour codes only, yellow = one glyph.
    gEdge[0] = Make(20.0, 60.0, 1, "");
    TextDrawUseBox(gEdge[0], 1);
    TextDrawTextSize(gEdge[0], 300.0, 78.0);
    TextDrawBoxColor(gEdge[0], 0x00FF0080);   // red, opaque enough to see

    gEdge[1] = Make(20.0, 85.0, 1, "        ");
    TextDrawUseBox(gEdge[1], 1);
    TextDrawTextSize(gEdge[1], 300.0, 103.0);
    TextDrawBoxColor(gEdge[1], 0x0000FF80);   // blue

    gEdge[2] = Make(20.0, 110.0, 1, "~r~~g~~b~~w~");
    TextDrawUseBox(gEdge[2], 1);
    TextDrawTextSize(gEdge[2], 300.0, 128.0);
    TextDrawBoxColor(gEdge[2], 0x00FF0060);

    gEdge[3] = Make(20.0, 135.0, 1, "X");
    TextDrawUseBox(gEdge[3], 1);
    TextDrawTextSize(gEdge[3], 300.0, 153.0);
    TextDrawBoxColor(gEdge[3], 0x00FFFF60);

    // control: identical settings, one visible ASCII character, so the box and
    // the row can be located on screen even if the empty rows do not draw.
    gEdge[4] = Make(20.0, 160.0, 1, "A");
    TextDrawUseBox(gEdge[4], 1);
    TextDrawTextSize(gEdge[4], 300.0, 178.0);
    TextDrawBoxColor(gEdge[4], 0xFFFF0080);   // yellow
    gLong = Make(20.0, 170.0, 1, " ", 0.35, 2.4);

    // same box settings, one pure ASCII (native path) and one with a marker
    gBox[0] = Make(20.0, 200.0, 1, "native box", 0.4, 1.2, 1, 1);
    TextDrawUseBox(gBox[0], 1);
    TextDrawTextSize(gBox[0], 240.0, 235.0);
    TextDrawBoxColor(gBox[0], 0x000000AA);
    gBox[1] = Make(300.0, 200.0, 1, " ", 0.4, 1.2, 1, 1);
    TextDrawUseBox(gBox[1], 1);
    TextDrawTextSize(gBox[1], 540.0, 235.0);
    TextDrawBoxColor(gBox[1], 0x000000AA);

    // font 4: txd sprite; the texture name is looked up in the client's txd
    // set, so several candidates are placed and whichever exists shows up.
    // TextSize is the sprite's box: without it there is nothing to stretch the
    // texture into and all that shows is the background colour.
    gSpr[0] = Make(300.0, 40.0, 4, "radar", 0.4, 1.6);
    TextDrawTextSize(gSpr[0], 60.0, 60.0);
    TextDrawBackgroundColor(gSpr[0], 0x00000000);

    gSpr[2] = Make(370.0, 40.0, 4, "SAMPHED", 0.4, 1.6);
    TextDrawTextSize(gSpr[2], 60.0, 60.0);
    TextDrawBackgroundColor(gSpr[2], 0x00000000);

    gSpr[3] = Make(440.0, 40.0, 4, "SAMP000", 0.4, 1.6);
    TextDrawTextSize(gSpr[3], 60.0, 60.0);
    TextDrawBackgroundColor(gSpr[3], 0x00000000);

    gSpr[4] = Make(510.0, 40.0, 4, "hud:radar_waypoint", 0.4, 1.6);
    TextDrawTextSize(gSpr[4], 60.0, 60.0);
    TextDrawBackgroundColor(gSpr[4], 0x00000000);

    // font 5: 3D model preview (411 = Infernus); TextSize is the preview box
    gSpr[1] = Make(580.0, 40.0, 5, " ");
    TextDrawTextSize(gSpr[1], 60.0, 60.0);
    TextDrawSetPreviewModel(gSpr[1], 411);
    TextDrawSetPreviewRot(gSpr[1], -10.0, 0.0, -20.0, 1.0);
    TextDrawBackgroundColor(gSpr[1], 0x00000000);

    // a plain ASCII TextDraw that the style 0 probe must not blank
    gGtKeep = Make(320.0, 300.0, 1, "TEXTDRAW MUST STAY", 0.5, 1.4, 0, 2);
    TextDrawAlignment(gGtKeep, 1);

    gMove = Make(100.0, 250.0, 1, "moving probe");
    gNum  = Make(100.0, 280.0, 1, "counter 0");

    print("[test] loaded - /td /td[bdjkrum] /tds /tdf /tdc /tdn /tdw /tdx /tdl /tdbox | /gt N [ms] /gtfix /spr /ptd /3d /move /num /menu /menu2 /menuoff /os /box /box2 /box3 /chat /spam [ms] /hide /lang /langgt");
    return 1;
}

public OnFilterScriptExit()
{
    for (new i = 0; i < 4; i++) { TextDrawDestroy(gTd[i]); TextDrawDestroy(gSize[i]); TextDrawDestroy(gFlags[i]); }
    for (new i = 0; i < 5; i++) { TextDrawDestroy(gEdge[i]); }
    for (new i = 0; i < 3; i++) { TextDrawDestroy(gWide[i]); }
    for (new i = 0; i < 5; i++) { TextDrawDestroy(gSpr[i]); }
    for (new i = 0; i < 2; i++) { TextDrawDestroy(gBreak[i]); }
    TextDrawDestroy(gColor);
    TextDrawDestroy(gLong);
    TextDrawDestroy(gBox[0]);
    TextDrawDestroy(gBox[1]);
    TextDrawDestroy(gGtKeep);
    TextDrawDestroy(gMove);
    TextDrawDestroy(gNum);
    if (gPtdCreated) for (new i = 0; i < 3; i++) PlayerTextDrawDestroy(0, gPtd[i]);
    if (gLabelCreated) Delete3DTextLabel(gLabel);
    if (gMoveOn) KillTimer(gMoveTimer);
    if (gNumOn)  KillTimer(gNumTimer);
    return 1;
}

static HideAll(playerid)
{
    for (new i = 0; i < 4; i++) {
        TextDrawHideForPlayer(playerid, gTd[i]);
        TextDrawHideForPlayer(playerid, gSize[i]);
        TextDrawHideForPlayer(playerid, gFlags[i]);
    }
    // gEdge holds 5 entries: the fifth is the one-glyph probe ("A"), which used to
    // stay visible when switching to another set.
    for (new i = 0; i < 5; i++) {
        TextDrawHideForPlayer(playerid, gEdge[i]);
    }
    for (new i = 0; i < 3; i++) {
        TextDrawHideForPlayer(playerid, gWide[i]);
        if (gPtdCreated) PlayerTextDrawHide(playerid, gPtd[i]);
    }
    for (new i = 0; i < 5; i++) { TextDrawHideForPlayer(playerid, gSpr[i]); }
    for (new i = 0; i < 2; i++) { TextDrawHideForPlayer(playerid, gBreak[i]); }

    TextDrawHideForPlayer(playerid, gColor);
    TextDrawHideForPlayer(playerid, gLong);
    if (gTdo != Text:0) TextDrawHideForPlayer(playerid, gTdo);   // /os sweep
    if (gBoxTd != Text:0) TextDrawHideForPlayer(playerid, gBoxTd); // /box
    TextDrawHideForPlayer(playerid, gBox[0]);
    TextDrawHideForPlayer(playerid, gBox[1]);
    TextDrawHideForPlayer(playerid, gGtKeep);
    TextDrawHideForPlayer(playerid, gMove);
    TextDrawHideForPlayer(playerid, gNum);
}

static SetAndShow(Text:t, playerid, const text[])
{
    TextDrawSetString(t, text);
    TextDrawShowForPlayer(playerid, t);
}

public MoveTick()
{
#if defined TextDrawSetPos
    TextDrawSetPos(gMove, float(gMoveX), 250.0);
#else
    TextDrawDestroy(gMove);
    gMove = Make(float(gMoveX), 250.0, 1, "moving probe");
    TextDrawShowForAll(gMove);
#endif
    gMoveX += 4;
    if (gMoveX > 500) gMoveX = 100;
}

public NumTick()
{
    new str[64];
    format(str, sizeof str, "counter %d", gNumTick++);
    TextDrawSetString(gNum, str);
    TextDrawShowForAll(gNum);
}

static ShowFonts(playerid, lang)
{
    gLang = lang;
    HideAll(playerid);

    for (new i = 0; i < 4; i++)
    {
        new str[300];
        format(str, sizeof str, "font %d [%s] %s", i, gLocale[lang], gLocal[lang]);
        SetAndShow(gTd[i], playerid, str);
    }
    return SendNote(playerid, gLocale[lang]);
}

forward SpamTick(playerid);
public SpamTick(playerid)
{
    if (playerid < 0 || playerid >= MAX_PLAYERS || !gSpamOn[playerid]) return 1;

    new msg[256];
    switch (random(8))
    {
        case 0: format(msg, sizeof msg, "[spam %d] random ascii line", random(10000));
        case 1: format(msg, sizeof msg, "[spam %d] \xD6\xD0\xCE\xC4\xB2\xE2\xCA\xD4\xCF\xFB\xCF\xA2", random(10000));
        case 2: format(msg, sizeof msg, "[spam %d] SAMP \xBB\xEC\xBA\xCF mixed \xC4\xDA\xC8\xDD", random(10000));
        case 3: format(msg, sizeof msg, "[spam %d] \xB1\xEA\xB5\xE3\xB7\xFB\xBA\xC5\xA3\xAC\xA1\xA3\xA3\xA1\xA3\xBF\xA3\xBA\xA3\xBB\xA1\xB0\xA1\xB1\xA1\xAF\xA3\xA8\xA3\xA9\xA1\xBE\xA1\xBF", random(10000));
        case 4: format(msg, sizeof msg, "[spam %d] \xCA\xFD\xD7\xD6 0123456789 \xBA\xCD\xD7\xD6\xC4\xB8 ABCdef", random(10000));
        case 5: format(msg, sizeof msg, "[spam %d] \xD5\xE2\xCC\xF5\xCF\xFB\xCF\xA2\xB1\xC8\xBD\xCF\xB3\xA4\xD3\xC3\xC0\xB4\xB2\xE2\xCA\xD4\xBB\xBB\xD0\xD0\xD3\xEB\xBF\xED\xB6\xC8\xCF\xDE\xD6\xC6\xA3\xAC\xBF\xB4\xBF\xB4\xBB\xE1\xB2\xBB\xBB\xE1\xB1\xBB\xBF\xCD\xBB\xA7\xB6\xCB\xBD\xD8\xB6\xCF\xBB\xF2\xD5\xDF\xD2\xE7\xB3\xF6\xA3\xAC\xD2\xBB\xB6\xFE\xC8\xFD\xCB\xC4\xCE\xE5\xC1\xF9\xC6\xDF\xB0\xCB\xBE\xC5\xCA\xAE", random(10000));
        case 6: format(msg, sizeof msg, "[spam %d] {FF8800}orange {FFFFFF}white {00FF00}green", random(10000));
        case 7: format(msg, sizeof msg, "[spam %d] !@#$%%^&*()_+-=[]{};:,.<>/?|~`", random(10000));
    }
    SendClientMessage(playerid, (random(0xFFFFFF) << 8) | 0xFF, msg);
    SetTimerEx("SpamTick", gSpamMs[playerid], false, "i", playerid);
    return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
    // ---- plain text per locale, fonts 0..3 ----
    // ---- client text scene ----
    // The node gamemode owns "/kill", and its natives are not registered on this server,
    // so a Pawn equivalent is needed to make the death window appear.
    // ---- every dialog style ----
    if (!strcmp(cmdtext, "/udlg", true, 5))
    {
        new style = 0;
        if (cmdtext[5] == ' ') style = cmdtext[6] - '0';
        if (style < 0 || style > 5) style = 0;
        return CTDialog(playerid, style);
    }

    // Both spellings: the node gamemode owns "/kill", and its natives are not
    // registered on this server, so a Pawn handler has to take the command.
    if (!strcmp(cmdtext, "/kill", true) || !strcmp(cmdtext, "/ukill", true))
    {
        SetPlayerHealth(playerid, 0.0);
        return SendNote(playerid, "kill");
    }

    // A death message for the death window: the server broadcasts the killer, the victim
    // and the weapon, and the client paints the row with the weapon icon. No second
    // player is needed, so the window can be checked alone.
    if (!strcmp(cmdtext, "/udeath", true))
    {
        SendDeathMessage(playerid, playerid, 24);          // deagle
        SendDeathMessage(playerid, playerid, 31);          // m4
        SendDeathMessage(playerid, playerid, 8);           // katana
        SendDeathMessage(playerid, playerid, 51);          // explosion
        return SendNote(playerid, "death messages sent - open the death window");
    }

    if (!strcmp(cmdtext, "/utest", true))  return CTBuildScene(playerid);
    if (!strcmp(cmdtext, "/uplate", true)) return CTPlate(playerid);
    if (!strcmp(cmdtext, "/uclean", true)) return CTClean(playerid);

    if (!strcmp(cmdtext, "/td", true))    return ShowFonts(playerid, 0);
    if (!strcmp(cmdtext, "/tdb", true))   return ShowFonts(playerid, 1);
    if (!strcmp(cmdtext, "/tdj", true))   return ShowFonts(playerid, 2);
    if (!strcmp(cmdtext, "/tdk", true))   return ShowFonts(playerid, 3);
    if (!strcmp(cmdtext, "/tdr", true))   return ShowFonts(playerid, 4);
    if (!strcmp(cmdtext, "/tdu", true))   return ShowFonts(playerid, 5);
    if (!strcmp(cmdtext, "/tdm", true))   return ShowFonts(playerid, 6);

    // ---- letter height sweep ----
    if (!strcmp(cmdtext, "/tds", true))
    {
        HideAll(playerid);
        for (new i = 0; i < 4; i++)
        {
            new str[300];
            format(str, sizeof str, "%s height %.1f | %s", gMark[gLang], 0.6 + 0.6 * i, gLocal[gLang]);
            SetAndShow(gSize[i], playerid, str);
        }
        return SendNote(playerid, "letter height 0.6 / 1.2 / 1.8 / 2.4");
    }

    // ---- shadow / outline / box ----
    if (!strcmp(cmdtext, "/tdf", true))
    {
        HideAll(playerid);
        new str[200];
        format(str, sizeof str, "%s shadow=0 outline=1", gMark[gLang]);
        SetAndShow(gFlags[0], playerid, str);
        format(str, sizeof str, "%s shadow=1 outline=0", gMark[gLang]);
        SetAndShow(gFlags[1], playerid, str);
        format(str, sizeof str, "%s shadow=1 outline=1", gMark[gLang]);
        SetAndShow(gFlags[2], playerid, str);
        format(str, sizeof str, "%s box + textsize 400x32", gMark[gLang]);
        SetAndShow(gFlags[3], playerid, str);
        return SendNote(playerid, "flags: outline / shadow / both / box");
    }

    // ---- outline / shadow sweep ----
    // /tdo <outline> <shadow>: one TextDraw with the sizes asked for, so the
    // renderer can be compared against the game's own limits.
    if (!strcmp(cmdtext, "/os", true, 3))
    {
        new s[32];
        new outline = 0;
        new shadow  = 0;
        new pos = 3;
        s[0] = EOS;

        while (cmdtext[pos] == ' ') pos++;                      // past the command
        outline = strval(cmdtext[pos]);
        while (cmdtext[pos] && cmdtext[pos] != ' ') pos++;      // past the first number
        while (cmdtext[pos] == ' ') pos++;
        if (cmdtext[pos]) shadow = strval(cmdtext[pos]);

        if (outline < 0) outline = 0;
        if (outline > 20) outline = 20;
        if (shadow < 0) shadow = 0;
        if (shadow > 20) shadow = 20;

        HideAll(playerid);
        if (gTdo != Text:0) { TextDrawDestroy(gTdo); gTdo = Text:0; }
        new str[120];
        format(str, sizeof str, "%s outline=%d shadow=%d \xB2\xE2\xCA\xD4", gMark[gLang], outline, shadow);
        gTdo = Make(20.0, 300.0, 1, str, 0.4, 1.6, shadow, outline);
        TextDrawShowForPlayer(playerid, gTdo);
        format(s, sizeof s, "outline=%d shadow=%d", outline, shadow);
        return SendNote(playerid, s);
    }
    // ---- a real box ----
    // TextDrawUseBox alone draws nothing: the box needs a colour with alpha and
    // a TextSize. For fonts 0..3 TextSize is the bottom-right CORNER of the box
    // (not a width/height), so both coordinates must be larger than the
    // TextDraw position. BackgroundColor is the dark edge behind the letters,
    // not the box.
    if (!strcmp(cmdtext, "/box", true))
    {
        HideAll(playerid);
        if (gBoxTd != Text:0) { TextDrawDestroy(gBoxTd); gBoxTd = Text:0; }
        gBoxTd = TextDrawCreate(100.0, 150.0, "box \xBA\xD0\xD7\xD3 test");
        TextDrawFont(gBoxTd, 1);
        TextDrawLetterSize(gBoxTd, 0.5, 1.6);
        TextDrawColor(gBoxTd, 0xFFFFFFFF);
        // Measured on this build: a transparent background suppresses the box
        // completely (box colour alone draws nothing, background colour alone
        // does), so both slots get the colour.
        TextDrawBackgroundColor(gBoxTd, 0x00FF0080);
        TextDrawSetShadow(gBoxTd, 0);
        TextDrawSetOutline(gBoxTd, 1);
        TextDrawTextSize(gBoxTd, 400.0, 250.0);    // bottom-right corner
        TextDrawUseBox(gBoxTd, 1);
        TextDrawBoxColor(gBoxTd, 0x00FF0080);      // half-transparent green
        TextDrawShowForPlayer(playerid, gBoxTd);
        return SendNote(playerid, "box: 100,150 to 400,250, green 0x00FF0080 (RRGGBBAA)");
    }
    // ---- box, the other way round ----
    // /box2 puts the same colour into TextDrawBackgroundColor instead of
    // TextDrawBoxColor: if this one shows a green box and /box does not, the
    // client is drawing the box from the background field.
    if (!strcmp(cmdtext, "/box2", true))
    {
        HideAll(playerid);
        if (gBoxTd != Text:0) { TextDrawDestroy(gBoxTd); gBoxTd = Text:0; }
        gBoxTd = TextDrawCreate(100.0, 150.0, "box2 \xBA\xD0\xD7\xD3 test");
        TextDrawFont(gBoxTd, 1);
        TextDrawLetterSize(gBoxTd, 0.5, 1.6);
        TextDrawColor(gBoxTd, 0xFFFFFFFF);
        TextDrawBackgroundColor(gBoxTd, 0x00FF0080);   // <- colour here
        TextDrawSetShadow(gBoxTd, 0);
        TextDrawSetOutline(gBoxTd, 1);
        TextDrawTextSize(gBoxTd, 400.0, 250.0);
        TextDrawUseBox(gBoxTd, 1);
        TextDrawShowForPlayer(playerid, gBoxTd);
        return SendNote(playerid, "box2: colour in BackgroundColor, UseBox on");
    }
    // ---- box, both fields set ----
    // /box3 sets the same colour in both slots, so it shows whether the pair of
    // them is enough regardless of which one the client reads.
    if (!strcmp(cmdtext, "/box3", true))
    {
        HideAll(playerid);
        if (gBoxTd != Text:0) { TextDrawDestroy(gBoxTd); gBoxTd = Text:0; }
        gBoxTd = TextDrawCreate(100.0, 150.0, "box3 \xBA\xD0\xD7\xD3 test");
        TextDrawFont(gBoxTd, 1);
        TextDrawLetterSize(gBoxTd, 0.5, 1.6);
        TextDrawColor(gBoxTd, 0xFFFFFFFF);
        TextDrawBackgroundColor(gBoxTd, 0x00FF0080);
        TextDrawSetShadow(gBoxTd, 0);
        TextDrawSetOutline(gBoxTd, 1);
        TextDrawTextSize(gBoxTd, 400.0, 250.0);
        TextDrawUseBox(gBoxTd, 1);
        TextDrawBoxColor(gBoxTd, 0x00FF0080);
        TextDrawShowForPlayer(playerid, gBoxTd);
        return SendNote(playerid, "box3: same colour in Background and BoxColor");
    }
    // ---- random chat flood ----
    // /spam [ms]  ->  start, default 500 ms; run it again to stop.
    if (!strcmp(cmdtext, "/spam", true, 5))
    {
        if (gSpamOn[playerid])
        {
            gSpamOn[playerid] = 0;
            return SendNote(playerid, "spam off");
        }
        new ms = strval(cmdtext[5]);
        if (ms < 100)  ms = 500;
        if (ms > 5000) ms = 5000;
        gSpamOn[playerid] = 1;
        gSpamMs[playerid] = ms;
        SetTimerEx("SpamTick", ms, false, "i", playerid);
        new note[64];
        format(note, sizeof note, "spam on, one random message every %d ms", ms);
        return SendNote(playerid, note);
    }

    // ---- colour codes ----
    if (!strcmp(cmdtext, "/tdc", true))
    {
        HideAll(playerid);
        new str[300];
        format(str, sizeof str,
               "%s ~r~R ~g~G ~b~B ~w~W ~y~Y ~p~P ~l~L ~h~H ~s~S {FF8800}hex {80FF0080}alpha ~x~literal", gMark[gLang]);
        SetAndShow(gColor, playerid, str);
        return SendNote(playerid, "colour codes, hex, alpha, unknown ~x~");
    }

    // ---- line breaks ----
    if (!strcmp(cmdtext, "/tdn", true))
    {
        HideAll(playerid);
        new str[300];
        format(str, sizeof str, "%s centre align: line 1~n~~r~line 2 is red~n~~g~line 3 is green~n~%s", gMark[gLang], gLocal[gLang]);
        SetAndShow(gBreak[0], playerid, str);

        format(str, sizeof str, "%s left align: line 1~n~~b~line 2 is blue~n~~w~line 3 is white~n~%s", gMark[gLang], gLocal[gLang]);
        SetAndShow(gBreak[1], playerid, str);
        return SendNote(playerid, "~n~ with centre and left alignment");
    }

    // ---- letter width sweep ----
    if (!strcmp(cmdtext, "/tdw", true))
    {
        HideAll(playerid);
        for (new i = 0; i < 3; i++)
        {
            new str[300];
            format(str, sizeof str, "%s letterW %.2f | %s", gMark[gLang],
                   (i == 0) ? 0.15 : ((i == 1) ? 0.35 : 0.70), gLocal[gLang]);
            SetAndShow(gWide[i], playerid, str);
        }
        return SendNote(playerid, "letterWidth 0.15 / 0.35 / 0.70, same height");
    }

    // ---- extended colour/special codes ----
    if (!strcmp(cmdtext, "/tdcode", true))
    {
        HideAll(playerid);
        new str[400];
        format(str, sizeof str,
               "%s~r~red ~r~~h~+h ~r~~h~~h~+hh ~g~green ~g~~h~+h ~b~blue ~w~white~n~~r~~h~~h~~h~~h~~h~very light red~n~arrows ~u~ ~d~ ~<~ ~>~ star ~]~~n~key ~k~~PED_FIREWEAPON~",
               gMark[gLang]);
        SetAndShow(gColor, playerid, str);
        return SendNote(playerid, "~h~ lightening, arrows, ~]~ star, ~k~ key");
    }

    // ---- the same idea in other languages (UTF-8; charset = auto accepts it) ----
    // Russian, Portuguese and Japanese are outside the client code page, so these
    // exercise the UTF-8 path and the font fallback rather than GBK.
    // ---- the same sentence in five languages, one textdraw, one line each ----
    // English is pure ASCII, so the client draws it with its own bitmap font: that
    // contrast is the point of the screenshot (the other four go through the mod).
    // Russian, Portuguese and Japanese are UTF-8, which charset = auto accepts.
    // ---- one command for a screenshot: textdraw on the right, native menu on the
    // left, GameText along the bottom. The English line is pure ASCII and therefore
    // drawn by the client itself; the rest goes through the mod, which is exactly the
    // contrast the screenshot is meant to show.
    // ---- one textdraw, one line per colour code, so a screenshot can be measured ----
    if (!strcmp(cmdtext, "/tdlight", true))
    {
        HideAll(playerid);
        SetAndShow(gLight, playerid,
            "~r~0~r~~h~1~r~~h~~h~2~r~~h~~h~~h~3~r~~h~~h~~h~~h~4~r~~h~~h~~h~~h~~h~5~n~~g~0~g~~h~1~g~~h~~h~2~g~~h~~h~~h~3~g~~h~~h~~h~~h~4~n~~b~0~b~~h~1~b~~h~~h~2~b~~h~~h~~h~3~n~~p~0~p~~h~1~p~~h~~h~2~n~~y~0~y~~h~1~y~~h~~h~2~n~~w~0~w~~h~1~n~~l~0~l~~h~1~l~~h~~h~2~n~~r~~h~~h~A~n~B~n~C");
        return SendNote(playerid, "light: a ~h~ ladder per colour, then A/B/C across ~n~");
    }

    if (!strcmp(cmdtext, "/tdstars", true))
    {
        HideAll(playerid);
        SetAndShow(gStarBg, playerid, " ");
        for (new i = 0; i < 4; i++) SetAndShow(gStars[i], playerid, "A~]~B");
        SetAndShow(gStarTd, playerid, "A~]~B");
        return SendNote(playerid, "star hunt: fonts 0,1,2,3 top to bottom");
    }

    if (!strcmp(cmdtext, "/tdglyphs", true))
    {
        HideAll(playerid);
        SetAndShow(gGlyphs, playerid, "~u~~n~~d~~n~~<~~n~~>~~n~~]~~n~A~u~B~d~C~<~D~>~E~]~F");
        return SendNote(playerid, "glyphs: up down left right star, then mixed with text");
    }

    if (!strcmp(cmdtext, "/tdcodes", true))
    {
        HideAll(playerid);
        SetAndShow(gKey, playerid, "~k~~PED_FIREWEAPON~");
        SetAndShow(gCodes, playerid, "~w~w  ABCDEF abcdef~n~~r~r  ABCDEF abcdef~n~~g~g  ABCDEF abcdef~n~~b~b  ABCDEF abcdef~n~~p~p  ABCDEF abcdef~n~~l~l  ABCDEF abcdef~n~~y~y  ABCDEF abcdef~n~~s~s  ABCDEF abcdef~n~~r~~h~r+h  ABCDEF~n~~r~~h~~h~r+2h  ABCDEF~n~~g~~h~g+h  ABCDEF~n~~b~~h~b+h  ABCDEF~n~~w~~h~w+h  ABCDEF~n~~y~~h~y+h  ABCDEF~n~~p~~h~p+h  ABCDEF~n~~l~~h~l+h  ABCDEF");
        return SendNote(playerid, "colour codes: 16 lines, one code each");
    }

    if (!strcmp(cmdtext, "/lang", true))
    {
        HideAll(playerid);
        SetAndShow(gLangTd, playerid, "\x54\x68\x65\x20\x71\x75\x69\x63\x6B\x20\x62\x72\x6F\x77\x6E\x20\x66\x6F\x78\x20\x6A\x75\x6D\x70\x73\x20\x6F\x76\x65\x72\x20\x74\x68\x65\x20\x6C\x61\x7A\x79\x20\x64\x6F\x67~n~\xE6\x95\x8F\xE6\x8D\xB7\xE7\x9A\x84\xE6\xA3\x95\xE8\x89\xB2\xE7\x8B\x90\xE7\x8B\xB8\xE8\xB7\xB3\xE8\xBF\x87\xE4\xBA\x86\xE9\x82\xA3\xE5\x8F\xAA\xE6\x87\x92\xE7\x8B\x97~n~\x41\x20\x72\xC3\xA1\x70\x69\x64\x61\x20\x72\x61\x70\x6F\x73\x61\x20\x6D\x61\x72\x72\x6F\x6D\x20\x73\x61\x6C\x74\x61\x20\x73\x6F\x62\x72\x65\x20\x6F\x20\x63\xC3\xA3\x6F\x20\x70\x72\x65\x67\x75\x69\xC3\xA7\x6F\x73\x6F~n~\xD0\x91\xD1\x8B\xD1\x81\xD1\x82\xD1\x80\xD0\xB0\xD1\x8F\x20\xD0\xBA\xD0\xBE\xD1\x80\xD0\xB8\xD1\x87\xD0\xBD\xD0\xB5\xD0\xB2\xD0\xB0\xD1\x8F\x20\xD0\xBB\xD0\xB8\xD1\x81\xD0\xB0\x20\xD0\xBF\xD1\x80\xD1\x8B\xD0\xB3\xD0\xB0\xD0\xB5\xD1\x82\x20\xD1\x87\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB7\x20\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB2\xD1\x83\xD1\x8E\x20\xD1\x81\xD0\xBE\xD0\xB1\xD0\xB0\xD0\xBA\xD1\x83~n~\xE3\x81\x99\xE3\x81\xB0\xE3\x82\x84\xE3\x81\x84\xE8\x8C\xB6\xE8\x89\xB2\xE3\x81\xAE\xE7\x8B\x90\xE3\x81\x8C\xE3\x81\xAA\xE3\x81\xBE\xE3\x81\x91\xE8\x80\x85\xE3\x81\xAE\xE7\x8A\xAC\xE3\x82\x92\xE9\xA3\x9B\xE3\x81\xB3\xE8\xB6\x8A\xE3\x81\x88\xE3\x82\x8B");

        if (gLangMenu == Menu:0)
        {
            gLangMenu = CreateMenu("hello \xE4\xBD\xA0\xE5\xA5\xBD", 1, 20.0, 80.0, 165.0, 0.0);
            AddMenuItem(gLangMenu, 0, "EN  Hello");
            AddMenuItem(gLangMenu, 0, "ZH  \xE4\xBD\xA0\xE5\xA5\xBD");
            AddMenuItem(gLangMenu, 0, "PT  Ol\xC3\xA1");
            AddMenuItem(gLangMenu, 0, "RU  \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
            AddMenuItem(gLangMenu, 0, "JA  \xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF");
        }
        ShowMenuForPlayer(gLangMenu, playerid);

        GameTextForPlayer(playerid, "Hello~n~\xE4\xBD\xA0\xE5\xA5\xBD~n~Ol\xC3\xA1~n~\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82~n~\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF", 30000, 5);   // style 5 = middle of the screen
        return SendNote(playerid, "textdraw top right, menu left, GameText middle");
    }
    if (!strcmp(cmdtext, "/langgt", true))
    {
        GameTextForPlayer(playerid, "\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88\xEF\xBC\x9A\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 5000, 3);
        return SendNote(playerid, "UTF-8 GameText: Japanese");
    }

    // ---- edge cases ----
    if (!strcmp(cmdtext, "/tdx", true))
    {
        HideAll(playerid);
        SetAndShow(gEdge[0], playerid, "");
        SetAndShow(gEdge[1], playerid, "        ");
        new str[200];
        format(str, sizeof str, "%s~r~~g~~b~~w~", gMark[gLang]);
        SetAndShow(gEdge[2], playerid, str);
        SetAndShow(gEdge[3], playerid, gMark[gLang]);
        SetAndShow(gEdge[4], playerid, "A");
        return SendNote(playerid, "empty / spaces / codes only / one glyph");
    }

    // ---- alpha probe: does a TextDraw's own alpha reach the screen? ----
    if (!strcmp(cmdtext, "/tda", true))
    {
        HideAll(playerid);
        if (gAlpha[0] == Text:0)
        {
            for (new i = 0; i < 4; i++)
            {
                gAlpha[i] = Make(60.0, 120.0 + 55.0 * i, 1, " ", 0.35, 1.40, 0, 0);   // no shadow, no outline
                TextDrawBackgroundColor(gAlpha[i], 0x00000000);   // nothing behind the glyphs
            }
            TextDrawColor(gAlpha[0], 0xFFFFFFFF);   // 255 opaque
            TextDrawColor(gAlpha[1], 0xFFFFFFC0);   // 192
            TextDrawColor(gAlpha[2], 0xFFFFFF80);   // 128
            TextDrawColor(gAlpha[3], 0xFFFFFF40);   //  64
        }
        new str[64];
        SetAndShow(gAlpha[0], playerid, "alpha 255  \xD6\xD0\xCE\xC4 ABC");
        SetAndShow(gAlpha[1], playerid, "alpha 192  \xD6\xD0\xCE\xC4 ABC");
        SetAndShow(gAlpha[2], playerid, "alpha 128  \xD6\xD0\xCE\xC4 ABC");
        SetAndShow(gAlpha[3], playerid, "alpha  64  \xD6\xD0\xCE\xC4 ABC");
        return SendNote(playerid, "text alpha 255 / 192 / 128 / 64, no shadow");
    }
    // ---- box probe: native vs ours ----
    if (!strcmp(cmdtext, "/tdbox", true))
    {
        HideAll(playerid);
        TextDrawShowForPlayer(playerid, gBox[0]);   // ASCII -> game draws it
        new str[200];
        format(str, sizeof str, "%s our box", gMark[gLang]);
        SetAndShow(gBox[1], playerid, str);         // marker -> we draw it
        return SendNote(playerid, "left = native box, right = our box");
    }

    // ---- over-long line: must be truncated, not dropped ----
    if (!strcmp(cmdtext, "/tdl", true))
    {
        HideAll(playerid);
        new str[900];
        str[0] = EOS;
        for (new i = 0; i < 10; i++) strcat(str, gLocal[gLang], sizeof str);
        SetAndShow(gLong, playerid, str);
        return SendNote(playerid, "very long line - check it is cut, not missing");
    }

    // ---- native menu (GTA CMenu) ----
    // Not covered by the plugin yet: does Chinese survive at all, or come out
    // as boxes/garbage? Either answer decides whether a hook is worth it.
    if (!strcmp(cmdtext, "/menu", true))
    {
        if (gMenu == Menu:0)
        {
            gMenu = CreateMenu("menu \xB2\xCB\xB5\xA5", 1, 120.0, 140.0, 180.0, 180.0);
            AddMenuItem(gMenu, 0, "item one \xB5\xDA\xD2\xBB\xCF\xEE");
            AddMenuItem(gMenu, 0, "item two \xB5\xDA\xB6\xFE\xCF\xEE");
            AddMenuItem(gMenu, 0, "~r~red~w~ \xBA\xEC green \xC2\xCC");
            AddMenuItem(gMenu, 1, "col2 \xB5\xDA\xB6\xFE\xC1\xD0");
            SetMenuColumnHeader(gMenu, 0, "col \xD2\xBB");
            SetMenuColumnHeader(gMenu, 1, "col \xB6\xFE");
        }
        ShowMenuForPlayer(gMenu, playerid);
        return SendNote(playerid, "native menu: watch the title and items - boxes? garbage? correct?");
    }
    // two columns, both filled: the case that also exercises column headers
    if (!strcmp(cmdtext, "/menu2", true))
    {
        if (gMenu2 == Menu:0)
        {
            gMenu2 = CreateMenu("menu \xB2\xCB\xB5\xA5 \xB6\xFE\xC1\xD0", 2, 120.0, 140.0, 180.0, 180.0);
            SetMenuColumnHeader(gMenu2, 0, "col \xD2\xBB");
            SetMenuColumnHeader(gMenu2, 1, "col \xB6\xFE");
            AddMenuItem(gMenu2, 0, "item one \xB5\xDA\xD2\xBB\xCF\xEE");
            AddMenuItem(gMenu2, 0, "item two \xB5\xDA\xB6\xFE\xCF\xEE");
            AddMenuItem(gMenu2, 0, "\xD5\xE2\xCA\xC7\xD2\xBB\xD0\xD0\xBA\xDC\xB3\xA4\xB5\xC4\xCE\xC4\xB1\xBE");
            AddMenuItem(gMenu2, 1, "col2 \xB5\xDA\xB6\xFE\xC1\xD0");
            AddMenuItem(gMenu2, 1, "~r~red~w~ \xBA\xEC ~g~green \xC2\xCC");
            AddMenuItem(gMenu2, 1, "12345 ABC");
        }
        ShowMenuForPlayer(gMenu2, playerid);
        return SendNote(playerid, "two-column menu");
    }
    // something else that draws text while a menu may be open
    if (!strcmp(cmdtext, "/chat", true))
    {
        SendClientMessage(playerid, 0xFFFFFFFF, "\xB2\xE2\xCA\xD4 \xD6\xD0\xCE\xC4 chat line");
        return 1;
    }
    if (!strcmp(cmdtext, "/menuoff", true))
    {
        if (gMenu != Menu:0) HideMenuForPlayer(gMenu, playerid);
        return SendNote(playerid, "menu hidden");
    }

    // ---- game text ----
    // /gtfix before /gt: the latter matches on the first three characters only
    if (!strcmp(cmdtext, "/gtfix", true))
    {
        HideAll(playerid);
        TextDrawShowForPlayer(playerid, gGtKeep);

        new str[200];
        format(str, sizeof str, "~y~style 0, 5 s~n~%s", gLocal[gLang]);
        GameTextForPlayer(playerid, str, 5000, 0);
        return SendNote(playerid, "5 s exactly (not 9), and the TextDraw above must stay visible");
    }

    if (!strcmp(cmdtext, "/gt", true, 3))
    {
        new style = strval(cmdtext[4]);
        if (style < 0 || style > 6)
        {
            SendClientMessage(playerid, 0xFFFFFFFF, "usage: /gt 0..6 [time_ms]   e.g. /gt 0 1500");
            return 1;
        }

        // optional second argument: the time to show it for
        new time = 5000;
        for (new i = 4; cmdtext[i]; i++)
        {
            if (cmdtext[i] == ' ')
            {
                new t = strval(cmdtext[i + 1]);
                if (t > 0) time = t;
                break;
            }
        }

        new str[400];
        format(str, sizeof str,
               "[%s] style %d~n~Hello, world~n~%s~n~~r~red ~g~green ~b~blue~n~arrows ~u~ ~d~ ~<~ ~>~ star ~]~ end~n~time %d ms",
               gLocale[gLang], style, gLocal[gLang], time);
        GameTextForPlayer(playerid, str, time, style);
        return 1;
    }

    // ---- pure-ASCII arrows: the client's own GameText rendering ----
    if (!strcmp(cmdtext, "/gta", true, 4))
    {
        new style = strval(cmdtext[4]);
        if (style < 0 || style > 6) style = 3;
        GameTextForPlayer(playerid, "arrows ~u~ ~d~ ~<~ ~>~ star ~]~ end", 5000, style);
        return 1;
    }

    // ---- font 4 + font 5: must stay untouched ----
    if (!strcmp(cmdtext, "/spr", true))
    {
        HideAll(playerid);
        for (new i = 0; i < 5; i++) TextDrawShowForPlayer(playerid, gSpr[i]);
        return SendNote(playerid, "font 4: radar/SAMPHED/SAMP000/hud:radar_waypoint | font 5: 411 preview");
    }

    // ---- player TextDraws ----
    if (!strcmp(cmdtext, "/ptd", true))
    {
        HideAll(playerid);
        if (!gPtdCreated)
        {
            for (new i = 0; i < 3; i++)
            {
                gPtd[i] = CreatePlayerTextDraw(playerid, 320.0, 300.0 + 25.0 * i, " ");
                PlayerTextDrawAlignment(playerid, gPtd[i], 1);
                PlayerTextDrawFont(playerid, gPtd[i], i + 1);
                PlayerTextDrawLetterSize(playerid, gPtd[i], 0.35, 1.4);
                PlayerTextDrawColor(playerid, gPtd[i], 0xFFFFFFFF);
                PlayerTextDrawSetOutline(playerid, gPtd[i], 1);
                PlayerTextDrawBackgroundColor(playerid, gPtd[i], 0x000000AA);
                PlayerTextDrawSetShadow(playerid, gPtd[i], 1);
            }
            gPtdCreated = true;
        }
        for (new i = 0; i < 3; i++)
        {
            new str[200];
            format(str, sizeof str, "[player %d] %s", i + 1, gLocal[gLang]);
            PlayerTextDrawSetString(playerid, gPtd[i], str);
            PlayerTextDrawShow(playerid, gPtd[i]);
        }
        return SendNote(playerid, "player TextDraws, fonts 1..3");
    }

    // ---- 3D text label ----
    if (!strcmp(cmdtext, "/3d", true))
    {
        new Float:x, Float:y, Float:z;
        GetPlayerPos(playerid, x, y, z);
        if (gLabelCreated) Delete3DTextLabel(gLabel);
        gLabel = Create3DTextLabel(gLocal[gLang], 0xFFFFFFFF, x, y, z + 0.8, 30.0, 0, 0);
        gLabelCreated = true;
        return SendNote(playerid, "3D label above you");
    }

    // ---- animation: position only ----
    if (!strcmp(cmdtext, "/move", true))
    {
        gMoveOn = !gMoveOn;
        if (gMoveOn) { TextDrawShowForAll(gMove); gMoveTimer = SetTimer("MoveTick", 50, true); }
        else         { KillTimer(gMoveTimer); TextDrawHideForAll(gMove); }
        return SendNote(playerid, gMoveOn ? "moving probe on" : "moving probe off");
    }

    // ---- animation: text changes ----
    if (!strcmp(cmdtext, "/num", true))
    {
        gNumOn = !gNumOn;
        if (gNumOn) { TextDrawShowForAll(gNum); gNumTimer = SetTimer("NumTick", 100, true); }
        else        { KillTimer(gNumTimer); TextDrawHideForAll(gNum); }
        return SendNote(playerid, gNumOn ? "counter on" : "counter off");
    }

    if (!strcmp(cmdtext, "/tdoff", true)) { HideAll(playerid); return 1; }

    // ---- selectable textdraws -------------------------------------------
    // Three selectable draws (one CJK, one ASCII, one CJK inside a box) plus a
    // command that puts the client into selection mode: clicking one has to
    // report the right id, and the hover highlight has to look like the client's.
    if (!strcmp(cmdtext, "/sel", true))
    {
        HideAll(playerid);

        if (gSel[0] == Text:0)
        {
            gSel[0] = TextDrawCreate(300.0, 240.0, "select me " L_GBK);
            TextDrawFont(gSel[0], 1);
            TextDrawLetterSize(gSel[0], 0.45, 1.55);
            TextDrawColor(gSel[0], 0xFFFFFFFF);
            TextDrawSetShadow(gSel[0], 0);
            TextDrawSetOutline(gSel[0], 0);
            TextDrawBackgroundColor(gSel[0], 0x000000AA);
            TextDrawSetSelectable(gSel[0], 1);

            gSel[1] = TextDrawCreate(300.0, 285.0, "second one, ascii only");
            TextDrawFont(gSel[1], 1);
            TextDrawLetterSize(gSel[1], 0.45, 1.55);
            TextDrawColor(gSel[1], 0xFFFFFFFF);
            TextDrawSetShadow(gSel[1], 0);
            TextDrawSetOutline(gSel[1], 1);
            TextDrawBackgroundColor(gSel[1], 0x000000AA);
            TextDrawSetSelectable(gSel[1], 1);

            gSel[2] = TextDrawCreate(300.0, 330.0, "boxed " L_GBK);
            TextDrawFont(gSel[2], 2);
            TextDrawLetterSize(gSel[2], 0.40, 1.40);
            TextDrawColor(gSel[2], 0xFFFFFFFF);
            TextDrawSetShadow(gSel[2], 0);
            TextDrawSetOutline(gSel[2], 1);
            TextDrawBackgroundColor(gSel[2], 0x000000AA);
            TextDrawTextSize(gSel[2], 600.0, 380.0);
            TextDrawUseBox(gSel[2], 1);
            TextDrawBoxColor(gSel[2], 0x000000AA);
            TextDrawSetSelectable(gSel[2], 1);
        }
        for (new i = 0; i < 3; i++) TextDrawShowForPlayer(playerid, gSel[i]);
        return SendNote(playerid, "3 selectable textdraws (cjk / ascii / boxed)");
    }

    // ---- punctuation probe ----------------------------------------------
    // Which ASCII characters does the client draw as something else (the star in
    // GameText is one: a literal ] comes out as a star)? One line per font, plus
    // /gtascii for any GameText style, so the mapping can be read off a screenshot.
    if (!strcmp(cmdtext, "/ascii", true))
    {
        HideAll(playerid);
        if (gAscii[0] == Text:0)
        {
            for (new i = 0; i < 4; i++)
            {
                gAscii[i] = TextDrawCreate(20.0, 30.0 + 80.0 * i, PUNCT);
                TextDrawFont(gAscii[i], i);
                TextDrawLetterSize(gAscii[i], 0.35, 1.30);
                TextDrawColor(gAscii[i], 0xFFFFFFFF);
                TextDrawSetShadow(gAscii[i], 0);
                TextDrawSetOutline(gAscii[i], 1);
                TextDrawBackgroundColor(gAscii[i], 0x000000AA);
            }
        }
        for (new i = 0; i < 4; i++) TextDrawShowForPlayer(playerid, gAscii[i]);
        return SendNote(playerid, "punctuation probe, fonts 0..3 top to bottom");
    }

    // Prefix match, like /gt and /gta: the exact compare this used to have meant
    // "/gtascii 3" never matched and the server answered "Unknown command".
    if (!strcmp(cmdtext, "/gtascii", true, 9))
    {
        new style = (cmdtext[8] == ' ') ? strval(cmdtext[9]) : strval(cmdtext[8]);
        if (style < 0 || style > 6)
        {
            SendClientMessage(playerid, 0xFFFFFFFF, "usage: /gtascii [0..6]   e.g. /gtascii 0");
            return 1;
        }
        GameTextForPlayer(playerid, PUNCT, 20000, style);
        return 1;
    }

    if (!strcmp(cmdtext, "/selshow", true))
    {
        SelectTextDraw(playerid, 0xFF0000AA);
        gSelMode[playerid] = true;
        return SendNote(playerid, "selection mode on: hover and click, right click cancels");
    }

    if (!strcmp(cmdtext, "/selhide", true))
    {
        CancelSelectTextDraw(playerid);
        gSelMode[playerid] = false;
        return SendNote(playerid, "selection mode off");
    }

    if (!strcmp(cmdtext, "/hide", true))
    {
        HideAll(playerid);
        if (gMoveOn) { KillTimer(gMoveTimer); gMoveOn = false; TextDrawHideForAll(gMove); }
        if (gNumOn)  { KillTimer(gNumTimer);  gNumOn  = false; TextDrawHideForAll(gNum); }
        if (gLabelCreated) { Delete3DTextLabel(gLabel); gLabelCreated = false; }
        if (gMenu != Menu:0) HideMenuForPlayer(gMenu, playerid);
        return 1;
    }
    return 0;
}

// Selectable textdraws: the id the client reports tells us whether the hit test
// still works with the mod installed (it is the client's own rect, computed in
// the renderer the mod still calls).
public OnPlayerClickTextDraw(playerid, Text:clickedid)
{
    if (clickedid == Text:INVALID_TEXT_DRAW)
    {
        gSelMode[playerid] = false;
        return SendNote(playerid, "click: cancelled");
    }

    new idx = -1;
    for (new i = 0; i < 3; i++) if (clickedid == gSel[i]) idx = i;

    new note[96];
    format(note, sizeof note, "click %d: selectable index %d", _:clickedid, idx);
    return SendNote(playerid, note);
}

// The client reports only the row index, so this cannot tell us how the text
// looked - it is here to confirm the menu is interactive with a hooked font.
public OnPlayerSelectedMenuRow(playerid, row)
{
    new note[64];
    format(note, sizeof note, "menu row %d selected", row);
    SendNote(playerid, note);
    HideMenuForPlayer(gMenu, playerid);
    return 1;
}

static SendNote(playerid, const note[])
{
    new msg[128];
    format(msg, sizeof msg, "[test] %s", note);
    SendClientMessage(playerid, 0xFFFFFFFF, msg);
    return 1;
}

// ---------------------------------------------------------------------------
// Client text scene: one of every surface the mod takes over, built next to the player.
// /utest builds it, /uplate changes the plate alone, /uclean removes it.
// ---------------------------------------------------------------------------
static gCTObject = INVALID_OBJECT_ID;
static gCTVehicle = INVALID_VEHICLE_ID;
static Text3D:gCTLabel;
static bool:gCTLabelSet = false;

static stock CTBuildScene(playerid)
{
    new Float:x, Float:y, Float:z, Float:a;
    GetPlayerPos(playerid, x, y, z);
    GetPlayerFacingAngle(playerid, a);

    if (gCTObject != INVALID_OBJECT_ID) DestroyObject(gCTObject);
    // An object's material text is rendered into a texture by the client.
    // Four metres ahead and three to the left, so it does not sit on top of the car.
    gCTObject = CreateObject(19482,
        x + (4.0 * floatsin(-a, degrees)) - (3.0 * floatcos(-a, degrees)),
        y + (4.0 * floatcos(-a, degrees)) + (3.0 * floatsin(-a, degrees)),
        z, 0.0, 0.0, 0.0);
    SetObjectMaterialText(gCTObject,
        "你好，世界！\nObject material text 测试",
        0, OBJECT_MATERIAL_SIZE_256x128,
        "Arial", 32, true, 0xFFFFFFFF, 0xFF000000,
        OBJECT_MATERIAL_TEXT_ALIGN_CENTER);

    if (gCTVehicle != INVALID_VEHICLE_ID) DestroyVehicle(gCTVehicle);
    // 411 (Infernus) has a plate on the rear bumper, and parking it facing away from
    // the player turns that bumper towards them.
    gCTVehicle = AddStaticVehicleEx(411,
        x + (4.0 * floatsin(-a, degrees)) + (3.0 * floatcos(-a, degrees)),
        y + (4.0 * floatcos(-a, degrees)) - (3.0 * floatsin(-a, degrees)),
        z + 0.2, a, 1, 1, -1);
    // The plate is another texture the client bakes.
    SetVehicleNumberPlate(gCTVehicle, "中文车牌");

    if (gCTLabelSet) Delete3DTextLabel(gCTLabel);
    gCTLabel = Create3DTextLabel("3D 文字标签\n你好，世界！", 0xFFFFFFFF, x, y, z + 2.5, 60.0, 0, 1);
    gCTLabelSet = true;

    // Chat bubble over the player: whether the player sees their own is what this asks.
    SetPlayerChatBubble(playerid, "聊天气泡 ChatBubble", 0xFFFFC800, 100.0, 20000);

    SendClientMessage(playerid, 0xFFFFFFFF, "chat: plain ASCII 123 ABC");
    SendClientMessage(playerid, 0xFFFFFFFF, "chat: {FF0000}red {00FF00}green {0000FF}blue {FFFFFF}white");
    SendClientMessage(playerid, 0xFFFFFF00, "聊天：你好，世界！这是中文测试");

    ShowPlayerDialog(playerid, 9080, DIALOG_STYLE_MSGBOX,
        "测试对话框 Dialog",
        "{BB9999}F1{BBBBBB}\tDisplay this help dialog\n\
         {BB9999}Tab{BBBBBB}\tDisplay the scoreboard\n\
         {BB9999}F7{BBBBBB}\tToggle the chat box\n\
         \n\
         {8080FF}你好，世界！欢迎来到服务器\n\
         {FFFFFF}Tab 分隔的两列应该对齐\t第二列",
        "确定", "取消");

    GameTextForPlayer(playerid, "~r~你好~w~世界~y~ GameText", 5000, 3);
    SendNote(playerid, "scene: object text, plate, 3D label, bubble, chat, dialog, gametext");
    return 1;
}

static stock CTPlate(playerid)
{
    if (gCTVehicle == INVALID_VEHICLE_ID) return SendNote(playerid, "run /utest first");
    SetVehicleNumberPlate(gCTVehicle, "中文车牌 A1");
    new Float:px, Float:py, Float:pz;
    GetVehiclePos(gCTVehicle, px, py, pz);
    SetVehiclePos(gCTVehicle, px, py, pz);        // makes the client rebuild the plate
    return SendNote(playerid, "plate: 中文车牌 A1");
}

static stock CTClean(playerid)
{
    if (gCTObject != INVALID_OBJECT_ID) { DestroyObject(gCTObject); gCTObject = INVALID_OBJECT_ID; }
    if (gCTVehicle != INVALID_VEHICLE_ID) { DestroyVehicle(gCTVehicle); gCTVehicle = INVALID_VEHICLE_ID; }
    if (gCTLabelSet) { Delete3DTextLabel(gCTLabel); gCTLabelSet = false; }
    return SendNote(playerid, "scene cleaned");
}

// Every dialog style, because they are not the same thing:
//   0 MSGBOX, 1 INPUT, 2 LIST, 3 PASSWORD are one column of text (a tab is just a tab),
//   4 TABLIST and 5 TABLIST_HEADERS are the only ones where \t makes real columns - the
//   client splits the line itself and draws each cell at its own x.
static stock CTDialog(playerid, style)
{
    new body[512];
    switch (style)
    {
        case 0:
        {
            format(body, sizeof body,
                "{BB9999}MSGBOX{BBBBBB}\t单列：这里的一个 tab 只是一个制表符\n\
                 {8080FF}你好，世界！欢迎来到服务器\n\
                 {FFFFFF}这一行足够长，用来看它会不会超出面板右边");
            ShowPlayerDialog(playerid, 9100, DIALOG_STYLE_MSGBOX, "对话框 MSGBOX 测试", body, "确定", "取消");
        }
        case 1:
        {
            format(body, sizeof body,
                "{8080FF}请输入一些中文：\n{FFFFFF}输入框的文字由客户端绘制（我们不接管输入）");
            ShowPlayerDialog(playerid, 9101, DIALOG_STYLE_INPUT, "对话框 INPUT 测试", body, "确定", "取消");
        }
        case 2:
        {
            format(body, sizeof body,
                "{FFFFFF}列表第一项\n{FF0000}列表第二项 红色\n{00FF00}列表第三项 绿色\n{8080FF}列表第四项 中文");
            ShowPlayerDialog(playerid, 9102, DIALOG_STYLE_LIST, "对话框 LIST 测试", body, "选择", "取消");
        }
        case 3:
        {
            format(body, sizeof body, "{FFFFFF}密码框，输入不显示");
            ShowPlayerDialog(playerid, 9103, DIALOG_STYLE_PASSWORD, "对话框 PASSWORD 测试", body, "确定", "取消");
        }
        case 4:
        {
            format(body, sizeof body,
                "{FF0000}第一列\t{00FF00}第二列\n\
                 {FFFFFF}名字 name\t{FFFFFF}数值 value\n\
                 {8080FF}你好世界\t{8080FF}第二格中文\n\
                 {FFFF00}一\t{FFFF00}二");
            ShowPlayerDialog(playerid, 9104, DIALOG_STYLE_TABLIST, "对话框 TABLIST 测试", body, "选择", "取消");
        }
        case 5:
        {
            format(body, sizeof body,
                "{FF0000}名字\t{00FF00}数值\n\
                 {FFFFFF}名字 name\t{FFFFFF}数值 value\n\
                 {8080FF}你好世界\t{8080FF}第二格中文\n\
                 {FFFF00}一\t{FFFF00}二");
            ShowPlayerDialog(playerid, 9105, DIALOG_STYLE_TABLIST_HEADERS, "对话框 TABLIST_HEADERS 测试", body, "选择", "取消");
        }
    }
    return SendNote(playerid, "dialog style 0..5: /udlg 0 1 2 3 4 5");
}

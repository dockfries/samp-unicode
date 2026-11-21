# Open review items

`src/` was read end to end once and the findings are tracked here so nothing is lost
between sessions; IDs (C8, B13, …) come from that review. Closed items are only
summarised under *History* - the commits carry the reasoning.

## Still open

| id | where | what goes wrong | fix | effort |
|---|---|---|---|---|
| **B14** | `hooks.cpp` `InMenuWindow` / `MenuSigKnown` | Menu takeover is a heuristic: any non-ASCII `CFont::PrintString` inside the menu window whose `(style, scaleX, scaleY)` matches one of up to six learned tuples is taken over. A chat line or HUD string that happens to share a tuple while a menu is open would be blanked and redrawn at the wrong pen. Never observed, not excluded either. | Require `InMenuObject(text)` (the string must live inside the `CMenu` object) instead of accepting the time window on its own; keep the window only for the panel's own copies. Do this only after counting how often each path matches. | small |
| **B20** | startup | Roughly one launch in three the client dies about a second in, before the mod installs any hook and with no crash dialog. Every event log entry is identical: `samp.dll_unloaded` offset `0xFFF7D`, `0xC0000005`. The same signature appears in events from before the encoding and shutdown work, so those did not introduce it. A vectored exception handler and an unhandled-exception filter both stayed silent, so it is not dispatched as an ordinary exception in the process - a fault inside the loader-lock window, or a stack overflow, are the remaining candidates. | Reproduce with the ASI renamed to `.off`: if the client still dies, it is the client or another mod. Otherwise dump the process from `DllMain` before the loader-lock work, or catch it with a debugger on first chance. | medium |

## Wanted: a logic self-test

`MenuWindowActive()` shipped inverted for one commit (`now - deadline` instead of
`deadline - now`). The compiler and CI both accepted it, and it only showed up as
mojibake in a real client. What is missing is an exported
`SampUnicodeSelfTest()` that checks the pure-logic pieces and is called by
`testhost` (which CI already runs):

- `MenuWindowActive()` around the tick wrap: never set, just set, about to
  expire, expired, deadline wrapped past 0xFFFFFFFF;
- `ParseColorRuns` on code-only, mixed and unknown-code strings (including `~]~`
  and `~k~~KEY~`, which must be dropped and expanded respectively);
- `WidthScale` against the expected stretch factor;
- the encoding tie-break for a byte pair that is valid in both UTF-8 and GBK
  (`D2 BB`), in `charset = auto` and in a forced code page.

Any failing case would have caught that commit.

## Release checklist

- [ ] Runtime pass on **0.3.7-R3-1** and **0.3.7-R5-1** (`/td`, `/tdc`, `/gt 2`,
      `/menu`); all seven builds match statically, only R1 and 0.3.DL have been
      exercised on a real client.
- [x] Scoreboard: `/gt 3 20000` + `/sel` + `/tdbox` + `/menu`, then TAB - the
      textdraws disappear entirely (the client skips its textdraw pass), the GameText
      stays visible *through* the panel, the menu is painted *below* it, and closing
      the panel restores everything. Confirmed on 0.3.7-R1 and 0.3.DL (the two builds
      the runtime passes were done on).
- [ ] Overlay layering: with `hook_point = endscene`, omp-client's F9 window covers
      the overlay; with `present` it does not. Check that `/menu`, `/move` and a
      pause (ESC) still behave in both modes.
- [ ] `~k~`: `/tdcodes` shows `key LMB`.
- [ ] `/spam 200` for a minute: chat must stay identical (the mod does not touch
      it) and the frame rate must not drop.
- [ ] First two log lines name the build (`mod loaded: …`,
      `samp-unicode <version> (build …)`).
- [ ] Alt-tab out and back once (device reset path), confirm the overlay returns.

## History

Closed, newest last; the commit has the reasoning.

- `729f5dd` / `272353f` - device vtable restored on unload, helper threads parked,
  originals assigned before patching, no allocation while threads are suspended,
  rel32 checked at every patch site, watchdog/attach/timer/tick-wrap/font-family
  batch (C1–C4, C7, C10, B1–B7, B9–B12, B15, B17–B19).
- `10b4433` - render state saved with a `D3DSBT_ALL` block, dead display-list
  parser removed (C8, C9).
- `7d99535` - outline pass before every body pass, `samp.dll` attach retried,
  `std::call_once` for the list lock, uniform trampoline guards, msbuild project
  (B13, B16, P9, P11, P12).
- `74d7241` - version stamp in the log, mask logs cover all 32 bits, one draw call
  per text item (P7).
- `59a9b2d` - per-cache budgets and logged clears, odd-length ini reported (P8, P10).
- `031062e` - detour calling conventions verified against the disassembly, no code
  change needed (C5, C6).
- Later sessions - `ParseFontFamily` walks every face of a `.ttc` (F1); text no
  longer stays behind on a frozen frame and the ESC flash is gone (K1, K2);
  `~h~` follows the client's ×1.5 rule; `~n~` replays the last colour code; the
  arrows use the client's own textures; `~]~` draws nothing; `size_divisor`
  defaults to 50.
- `e4d3201` - the horizontal fit counts drawn glyphs instead of bytes (W1): the
  byte count mis-sized every page that uses colour codes.
- This session - menu title calibrated against the client (`title_width`, the tall
  face); `gametext.scale` default 0.82 from a measured cap height; the scoreboard
  draws the GameText layer *behind* the panel (`CScoreboard::Draw` hook) and drops
  the TextDraw layer the way the client does; the client's selection highlight pass
  is hidden through its own `+0x9D2` slot; face 2's star punctuation is reproduced
  (`font.client_glyphs`); an optional `EndScene` hook point so other overlays can be
  drawn above the overlay.

- This session, later - `[textdraw] hide` removed: the `spaces` way of hiding the
  client's glyphs replaced the string, which moved the hit rectangle of a
  selectable textdraw. Only the alpha path is left, so the string is never
  touched (B21).

Deliberately kept: the menu display-list encoder hook. Nothing depends on it any
more (`CMenu::Show` is hooked on its own), it is installed best-effort and only
counts calls for `debug.verbose`. Removing it is a cosmetic diff, not a fix.

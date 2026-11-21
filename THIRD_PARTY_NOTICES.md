# Third-party notices

This project vendors the following third-party components. Their licenses are
reproduced at the paths given below and must be kept with the source tree and
with any binary distribution.

## compact_enc_det

- **What it is**: a compact character-set detector used to guess the encoding
  of text sent by the server when `[encoding] charset = auto`.
- **Upstream**: https://github.com/google/compact_enc_det
- **License**: Apache License 2.0
- **Vendored copy**: `third_party/compact_enc_det/`
- **License text**: `third_party/compact_enc_det/LICENSE`
- **Modifications**: none. The files are copied unmodified from upstream
  (`compact_enc_det/` and `util/` subtrees only, tests and build files omitted).

## Pirata One (font)

- **What it is**: the blackletter face shipped as `font0.ttf`, used for
  TextDrawFont 0 and the GameText styles that use that id (style 2, the "wasted"
  look). The stock game draws those with a bitmap blackletter; Windows has no
  such font, so this one is bundled instead of falling back to a serif.
- **Upstream**: https://fonts.google.com/specimen/Pirata+One
- **License**: SIL Open Font License 1.1, Copyright (c) 2012 Rodrigo Fuenzalida
  and Nicolas Massi, with Reserved Font Name 'Pirata'
- **Vendored copy**: `third_party/fonts/PirataOne-Regular.ttf`
- **License text**: `third_party/fonts/LICENSE`
- **Modifications**: none. The release zip only renames the file to
  `font0.ttf`; the font data and its internal names are unchanged.

## Windows components

The plugin links against system libraries only:

- `kernel32`, `user32`, `gdi32`, `version` — no redistribution required.

## Fonts

The plugin can use a font file supplied by the user (`[font] file`, or a
`fontN.ttf` / `font0.ttf` … `font3.ttf` next to it). The only font shipped with
this project is Pirata One above; every other face is resolved from fonts
already installed on the system (Arial, Impact, Bahnschrift, …), which are not
redistributed here.

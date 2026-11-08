# ASCII viewmodel

`weapon-glyphs.txt` and `weapon-colors.txt` are matching 100 × 56 cell layers for the foreground rifle and gloves. Each glyph row ends in `|` solely as a line terminator, preserving intentional spaces. The colour layer uses `_` for transparency and `0`–`9`, `A`–`Z`, `a`–`z` as palette indices into `web/app.js`. The Rust renderer draws these cells over the ray-cast room; no raster image is loaded at runtime.

# Platform boundary and targets

**Status: M0 complete; platform implementations are planned.** The platform layer is a small C ABI for window/input polling, presenting pixels, file access, and pacing. Lisp owns application behavior and the choice of what to draw. The software rasterizer is shared policy-free code; backend code adapts host facilities to it.

## Planned targets

| Target | Backend | Planned milestone |
|---|---|---:|
| Linux and macOS headless | `platform/headless` | M7 |
| Linux and macOS desktop window | `platform/sdl2` | M10 |
| Browser via WebAssembly | `platform/wasm` | M11 |
| PSP, using PSPSDK; PPSSPP is the intended emulator for validation | `platform/psp` | M14 |

The M14 device choice is PSP. M13's richer UI work is deferred; M14 does not depend on completing M13. The initial built-in font target is an 8×8 ASCII bitmap font, with a fallback glyph for characters outside the font. Wider Unicode and TTF baking remain later work.

## Portability and determinism

The kernel targets portable C99 and a fixed-size arena, with no `malloc` after initialization. Platform APIs should remain thin and avoid leaking host-specific behavior into Lisp-visible semantics. Time advances by explicit frames, not wall-clock time. Exact integer layout, raster, and replay results are required across backends; floating-point results may differ across platforms as noted in `docs/decisions.md`.

Keep this page synchronized with the actual ABI when M7 introduces `platform.h`, and update backend capabilities only when they exist and have been exercised.

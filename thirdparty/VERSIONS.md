# Vendored third-party code

Everything under `thirdparty/` is copied in, not fetched at build time.
This file records exactly what was copied so the next upgrade can diff
against the right upstream commit.

| Library | Upstream | Vendored commit / tag | Date | Local changes |
|---|---|---|---|---|
| litehtml | https://github.com/litehtml/litehtml | `69254c75c415` (master, after v0.10) | 2026-11-17 | yes — see `litehtml/PATCHES.md` |
| lunasvg | https://github.com/sammycage/lunasvg | `470e78e2953d` (master, after v3.5.0) | 2026-10-21 | none |
| plutovg | bundled inside lunasvg (`lunasvg/plutovg/`) | as shipped by the lunasvg commit above (v1.3.3) | — | none |
| md4c | https://github.com/mity/md4c | `v0.6.0` (`md4c.c`, `md4c.h`, `md4c-html.c`, `md4c-html.h`, `entity.c`, `entity.h` from `src/`; its `LICENSE.md` as `md4c-LICENSE.md`) | 2026 | none |

What was left out when copying: `.git*`, `.github/`, upstream CI/lint
config, `examples/`, `tests/`, Meson files and sample images. Build logic
for litehtml and md4c lives in the top-level `CMakeLists.txt`; lunasvg
and plutovg are built through their own `CMakeLists.txt` via
`add_subdirectory`.

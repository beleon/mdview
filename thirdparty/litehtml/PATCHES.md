# Local patches to litehtml

The litehtml tree under `thirdparty/litehtml/` is **not** stock upstream —
it carries a small local patch that mdview relies on. Anyone upgrading
litehtml needs to re-apply (or verify the equivalent of) what's listed
here.

## `LITEHTML_UTF8_ONLY` compile switch

Adds a build-time switch that lets the application opt out of litehtml's
character-encoding-conversion path entirely. Upstream litehtml uses
`iconv` (and depending on flags, `libxml2`) to convert non-UTF-8 source
documents to UTF-8 internally; mdview only ever feeds litehtml UTF-8
output from md4c, so we don't need that machinery — and dropping it
means we don't have to link `iconv`.

When the switch is defined the document/CSS parsers treat input as
UTF-8 verbatim and `encodings.cpp` is excluded from the build entirely
(via `list(FILTER ... EXCLUDE encodings\\.cpp$)` in the top-level
`CMakeLists.txt`).

**Files touched in this tree:**

- `CMakeLists.txt` (~line 175) — adds the `LITEHTML_UTF8_ONLY` compile
  option, wired to `target_compile_definitions(... PUBLIC ...)`.
- `include/litehtml/encodings.h` — the whole declarations file is
  gated behind `#ifndef LITEHTML_UTF8_ONLY`.
- `src/document.cpp` (~line 153) — encoding-detection / conversion
  block bypassed under the switch; UTF-8 input passes straight through.
- `src/css_parser.cpp` (~line 150) — same idea for CSS sources.

The mdview side flips the switch (and excludes `encodings.cpp`) in
the top-level `CMakeLists.txt`:

```cmake
list(FILTER LITEHTML_SRC EXCLUDE REGEX "encodings\\.cpp$")
target_compile_definitions(litehtml PUBLIC LITEHTML_UTF8_ONLY)
```

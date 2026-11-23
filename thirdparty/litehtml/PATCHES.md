# Local patches to litehtml

The litehtml tree under `thirdparty/litehtml/` is **not** stock upstream —
it carries a small local patch that mdview relies on. Anyone upgrading
litehtml needs to re-apply (or verify the equivalent of) what's listed
here. The upstream commit this tree was taken from is recorded in
`thirdparty/VERSIONS.md`.

## `LITEHTML_UTF8_ONLY` compile switch

Adds a build-time switch that lets the application opt out of litehtml's
character-encoding-conversion path entirely. Upstream litehtml converts
non-UTF-8 source documents to UTF-8 internally (`src/encodings.cpp`,
with its own decoder tables for the legacy encodings); mdview only ever
feeds litehtml UTF-8 output from md4c, so we don't need that machinery.

When the switch is defined the document/CSS parsers treat input as
UTF-8 verbatim and `encodings.cpp` is excluded from the build entirely
(via `list(FILTER ... EXCLUDE encodings\\.cpp$)` in the top-level
`CMakeLists.txt`).

**Files touched in this tree:**

- `CMakeLists.txt` — adds the `LITEHTML_LEGACY_ENCODINGS` option (default
  OFF); when off, `src/encodings.cpp` is dropped from the source list and
  `LITEHTML_UTF8_ONLY` is defined `PUBLIC` on the target. Only matters if
  litehtml is built through its own CMake; mdview globs the sources itself.
- `include/litehtml/encodings.h` — under `LITEHTML_UTF8_ONLY` the file
  declares only a minimal `estring` (a `std::string` wrapper with the
  constructors `document::createFromString` needs); the real declarations
  are gated behind `#else`.
- `src/document.cpp` — `document::parse_html()` is replaced by a version
  that hands the bytes straight to gumbo; `adjust_meta_encoding()`,
  `get_meta_encoding()` and the sniffing `parse_html()` are gated behind
  `#else`.
- `src/css_parser.cpp` — the `decode(input, encoding::utf_8)` repair pass
  at the top of `parse_stylesheet()` is bypassed; input is used as-is.

The mdview side flips the switch (and excludes `encodings.cpp`) in
the top-level `CMakeLists.txt`:

```cmake
list(FILTER LITEHTML_SRC EXCLUDE REGEX "encodings\\.cpp$")
target_compile_definitions(litehtml PUBLIC LITEHTML_UTF8_ONLY)
```

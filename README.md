<p align="center">
  <img src="icons/mdview-256.png" width="128" alt="mdview app icon">
</p>

<h1 align="center">mdview</h1>

<p align="center">
  <strong>A quick-look Markdown viewer for the Wayland desktop.</strong><br>
  Open a README without breaking flow. Launches fast, doesn't pin your terminal, gets out of your way.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT License"></a>
  <img src="https://img.shields.io/badge/platform-Wayland-ff9800" alt="Wayland">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C" alt="C++17">
  <img src="https://img.shields.io/badge/deps-stock%20Wayland%20session-4CAF50" alt="Stock Wayland deps">
</p>

<p align="center">
  <img src="docs/screenshots/hero.png" alt="bat README.md (left) vs mdview README.md (right)" width="600">
</p>

---

## Features

- Near-instant startup for quick looks at `.md` files
- Native Wayland window that gets out of your way
- Detaches from the shell by default, so your terminal stays free
- Real Markdown rendering: headings, tables, links, images, anchors, and highlighted code blocks
- Find, live reload, dark mode, zoom, text selection, and outline sidebar
- Keyboard, mouse, and touchpad-friendly navigation
- No GTK, Qt, Electron, Tauri, browser engine, or XWayland fallback

## Why mdview

Developers swim in `.md` files: READMEs, design notes, runbooks, the
random `NOTES.md` in every project. You're in the terminal. You want to
actually *read* one. The options aren't great:

- **`less` / `vim`** — fine for skimming, but it's raw text. No heading
  hierarchy, no tables, no anchors, no code-block highlighting.
- **`pandoc` / `asciidoctor` → browser** — too many steps for a glance.
  By the time you've set it up the moment's gone.
- **Electron viewers** — slow to launch, heavy on memory, often tie up
  the terminal you ran them from.

mdview is the missing middle. It opens in milliseconds, **detaches from
the launching shell** so your terminal is free, and gives you a real
renderer with syntax-highlighted code, an outline sidebar, live reload,
dark mode, and a proper find bar. Run it side-by-side with your terminal;
treat it like `less` for `.md` files.

The cost is low enough that you'll actually use it.

## Built tiny on purpose

- **Native Wayland.** First-class `xdg-shell`, `wp_cursor_shape_v1`,
  `xdg-toplevel-icon-v1`, `pointer-gestures-unstable-v1`,
  `xdg-decoration-unstable-v1`. No XWayland fallback.
- **Frameworkless.** No GTK, Qt, Electron, Tauri, or browser engine.
  The whole UI is direct Cairo draw calls onto a `wl_shm` buffer.
- **Drops into any modern Wayland desktop.** GNOME, KDE Plasma, Sway,
  Hyprland. Only links against libraries already loaded in every Wayland
  session.

```
  .md file ──▶ md4c (Markdown→HTML) ──▶ litehtml (HTML/CSS→layout) ──▶ Cairo+Pango (render)
                                                                            │
                                                                     ┌──────┘
                                                                     ▼
                                                         libwayland-client + xdg-shell
                                                         (window, input, shared memory)
```

## Showcase

<table>
  <tr width="700">
    <td align="center" width="350">
      <img src="docs/screenshots/find.png" alt="Find bar in light mode" width="300"><br>
      <sub><b>Find bar (light mode).</b> Case / whole-word / regex, full text-cursor editing.</sub>
    </td>
    <td align="center" width="350">
      <img src="docs/screenshots/outline.png" alt="Outline sidebar in dark mode" width="300"><br>
      <sub><b>Outline sidebar (dark mode).</b> <code>Ctrl+T</code>. h1–h3, click to jump.</sub>
    </td>
  </tr>
</table>

## Build

### Quick — Podman

```bash
./build.sh
```

Produces `dist/mdview`. The binary is self-contained: icons baked in, no runtime data files.

### Or with system CMake

```bash
sudo apt install libwayland-dev libwayland-cursor0 wayland-protocols libxkbcommon-dev \
                 libcairo2-dev libpango1.0-dev libgdk-pixbuf-2.0-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

Requires CMake ≥ 3.16 and a C++17 compiler.

### Install

```bash
./mdview --install      # writes binary, icons, .desktop entry under ~/.local/
./mdview --uninstall    # undoes the above
```

`--install` is XDG-compliant: binary lands in `~/.local/bin`, icons go under `~/.local/share/icons/hicolor/<size>/apps/`, the `.desktop` file under `~/.local/share/applications/`.

## Usage

```bash
mdview path/to/document.md
```

### Flags

| Flag | Action |
|------|--------|
| `--no-highlight` | Disable syntax highlighting of code blocks |
| `--no-reload` | Disable live-reload on file change |
| `-f` / `--foreground` | Stay attached to the launching terminal |
| `--install` | Install binary + icons + `.desktop` under `~/.local/` |
| `--uninstall` | Undo `--install` |
| `-h` / `--help` | Show usage |

Env vars:

- `MDVIEW_NO_HIGHLIGHT=1` suppresses syntax highlighting globally.
- `MDVIEW_DEBUG=1` prints a startup perf report (phase timings, RSS,
  doc stats) to stderr at first paint. Combine with `-f` so stderr
  stays attached to your terminal: `MDVIEW_DEBUG=1 mdview -f doc.md`.

### Keyboard

| Key | Action |
|-----|--------|
| `j` / `↓` | Scroll down |
| `k` / `↑` | Scroll up |
| `Space` / `PageDown` | Page down |
| `Shift+Space` / `PageUp` | Page up |
| `g` / `Home` | Top of document |
| `G` / `End` | Bottom of document |
| `Ctrl+C` | Copy selection to clipboard |
| `Ctrl+A` | Select all text |
| `Ctrl+F` | Open find bar |
| `Enter` / `F3` | Next match (while find bar is open) |
| `Shift+Enter` / `Shift+F3` | Previous match |
| `Alt+C` / `Alt+W` / `Alt+R` | Toggle case-sensitive / whole-word / regex (in find bar) |
| `Ctrl+Backspace` / `Ctrl+W` | Delete word (in find bar) |
| `Ctrl++` / `Ctrl+=` | Increase text size (reflows) |
| `Ctrl+-` | Decrease text size |
| `Ctrl+0` | Reset text size + zoom |
| `Ctrl+T` | Toggle outline (TOC) sidebar |
| `Ctrl+D` | Toggle dark mode |
| `F1` / `?` | Toggle help overlay |
| `q` | Quit |
| `Escape` | Close find / help / context menu (doesn't quit) |

### Mouse

- **Wheel** — scroll
- **Shift+wheel** — horizontal scroll
- **Ctrl+wheel** — zoom
- **Pinch (touchpad)** — zoom
- **Click + drag** — select text · **double-click** word · **triple-click** line
- **Click on link** — open in browser, or scroll to `#anchor`
- **Right-click** — context menu (Open Link / Copy / Select All / Find / Reload / Toggle Dark)
- **Title bar drag** — move window (CSD mode) · **double-click** to toggle maximize

## Configuration

mdview keeps two files under `~/.config/mdview/`:

- **`state`** — machine-managed. Per-file scroll / zoom / text-size LRU (hashed paths, last 20 files), last window size, dark mode, search flags. Rewritten on every close; don't edit by hand.
- **`config`** — user-editable. Defaults for newly-opened files only; the LRU always wins for files you've already adjusted. Optional: if missing, built-in defaults apply.

```ini
# ~/.config/mdview/config
font_size = 16    # body font-size in px (8..48)
zoom      = 1.0   # pinch-zoom level, 1.0 = 100% (1.0..5.0)
```

`#` comments and whitespace around `=` are tolerated.

## Window decorations

mdview negotiates server-side decorations via `xdg-decoration-unstable-v1`. Compositors that support it (KDE Plasma, Sway, Hyprland, …) draw their own title bar. If the compositor doesn't support the protocol or requests client-side decorations (e.g. GNOME/Mutter), mdview draws a minimal dark title bar with a close button.

## Runtime dependencies

Only libraries already loaded in any Wayland session:

| Library | Why |
|---|---|
| `libwayland-client.so` | Wayland protocol |
| `libwayland-cursor.so` | Cursor theme; only loaded on compositors without `wp_cursor_shape_v1` |
| `libxkbcommon.so` | Keyboard layout & keysyms |
| `libcairo.so` | 2D rendering |
| `libpangocairo-1.0.so` / `libpango-1.0.so` | Text shaping & layout |
| `libgdk_pixbuf-2.0.so` | Raster image loading (PNG/JPG/GIF) |

SVG support is provided by [lunasvg](https://github.com/sammycage/lunasvg)
+ plutovg, vendored under `thirdparty/lunasvg/` and statically linked. No
runtime dependency on librsvg / glib-gobject / libxml2 / ICU.

## Acknowledgements

Designed by [Leon Becker](https://blog.leonbecker.de); implementation
largely by [Claude Code](https://claude.com/claude-code). The project's
structure, scope, and design decisions are mine; most of the code itself
was produced by Claude under my direction.

## License

mdview is MIT-licensed. See [LICENSE](LICENSE).

Bundled / linked dependencies:

| Component | License |
|---|---|
| litehtml | BSD-3-Clause |
| md4c | MIT |
| lunasvg, plutovg | MIT |
| Cairo, Pango, Wayland | LGPL |

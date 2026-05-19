# mdview

A lightweight Wayland-native Markdown viewer. No frameworks — raw `libwayland-client` + Cairo + Pango + litehtml + md4c.

Takes a single CLI argument — a path to a `.md` file — converts it to HTML via md4c, and renders it in a native Wayland window using litehtml's HTML/CSS engine with Cairo for 2D drawing and Pango for text.

## Architecture

```
  .md file ──▶ md4c (Markdown→HTML) ──▶ litehtml (HTML/CSS→layout) ──▶ Cairo+Pango (render)
                                                                            │
                                                                     ┌──────┘
                                                                     ▼
                                                         libwayland-client + xdg-shell
                                                         (window, input, shared memory)
```

**< 5k lines of our code, zero frameworks — and a lot fits in there:** find-in-doc with case / whole-word / regex, async syntax highlighting, dark mode, outline sidebar, live reload, right-click menu, persistent state, HiDPI rendering, pinch zoom, themed cursors, embedded app icon, self-installer.

## Features

- Native Wayland window with server-side decoration negotiation (CSD fallback with title bar + close button)
- Character-level text selection via mouse drag with Pango hit testing
- Copy to clipboard via **Ctrl+C** (Wayland `wl_data_source`)
- Select all via **Ctrl+A**
- Find-in-document via **Ctrl+F** with match highlighting and navigation
- Zoom via **Ctrl+wheel**, **Ctrl++ / Ctrl+- / Ctrl+0**, or trackpad pinch
- Keyboard navigation (vim-style and standard)
- Smooth scrolling via mouse wheel
- Minimal scrollbar overlay
- Full CSS layout via litehtml (headings, paragraphs, code blocks, tables, lists, blockquotes, horizontal rules)
- Async syntax highlighting of code blocks via `bat` / `pygmentize` / `highlight` / `source-highlight` (whichever is installed); pass `--no-highlight` or set `MDVIEW_NO_HIGHLIGHT=1` to disable
- Click links to open in your default browser (`xdg-open`), or in-document `#anchor` links to scroll to that heading
- Live reload on file change (inotify); disable with `--no-reload`
- Persistent state (zoom, window size, dark mode, search flags) between sessions (`~/.config/mdview/state`)
- Help overlay with **F1** or **?**
- Status bar showing filename, scroll %, zoom %, and highlight progress
- Outline / TOC sidebar (Ctrl+T) listing h1-h3 headings; click to jump
- Dark mode (Ctrl+D), persisted
- Right-click context menu (Open Link / Copy Link URL / Copy / Select All / Find / Reload / Toggle Dark)
- Whole-word and regex search modes (Alt+W / Alt+R) alongside case-sensitive (Alt+C)
- HiDPI-aware rendering via `wl_surface.preferred_buffer_scale` (integer scales)
- Detaches from the launching terminal (VS Code style); `-f` / `--foreground` keeps it attached
- Themed cursors via `wp_cursor_shape_v1` (compositor draws the user's actual cursor theme); falls back to `wl_cursor_theme` on older compositors
- App icon embedded in the binary and announced via `xdg_toplevel_icon_v1` on compositors that support it
- Self-installer: `mdview --install` lays out the binary, icons, and `.desktop` entry under `~/.local/` (XDG-compliant); `--uninstall` reverses it

## Building

### Prerequisites

- CMake ≥ 3.16, C++17 compiler
- `libwayland-dev libwayland-cursor0 wayland-protocols libxkbcommon-dev libcairo2-dev libpango1.0-dev libgdk-pixbuf-2.0-dev librsvg2-dev`

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Build with Podman

```bash
./build.sh
```

Produces `dist/mdview`. The binary is self-contained — `dist/mdview --install` lays out the binary, icons, and `.desktop` entry under `~/.local/`.

## Usage

```bash
./mdview path/to/document.md
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
| `Ctrl+F` | Open find bar (Esc to close) |
| `Enter` / `F3` | Next match (while find bar is open) |
| `Shift+Enter` / `Shift+F3` | Previous match |
| `Alt+C` | Toggle case-sensitive search (while find bar is open) |
| `Alt+W` | Toggle whole-word search (while find bar is open) |
| `Alt+R` | Toggle regex search (while find bar is open) |
| `Ctrl+Backspace` | Delete word in find bar |
| `Ctrl++` / `Ctrl+=` | Zoom in |
| `Ctrl+-` | Zoom out |
| `Ctrl+0` | Reset zoom |
| `Ctrl+T` | Toggle outline (TOC) sidebar |
| `Ctrl+D` | Toggle dark mode |
| `F1` / `?` | Toggle help overlay |
| `q` / `Escape` | Quit (Esc also closes help / find) |

### Mouse

- **Scroll wheel** — scroll document
- **Shift+wheel** — horizontal scroll
- **Ctrl+wheel** — zoom in / out
- **Trackpad pinch** — zoom in / out (compositors that support `pointer-gestures-unstable-v1`)
- **Click + drag** — select text
- **Double-click** — select word (drag to extend by words)
- **Triple-click** — select line (drag to extend by lines)
- **Click on link** — open in browser, or scroll to in-doc anchor
- **Right-click** — context menu (Open Link / Copy / Select All / Find / Reload / Toggle Dark)
- **Title bar drag** — move window (CSD mode)

## Window Decorations

mdview negotiates server-side decorations via the `xdg-decoration-unstable-v1` protocol. Compositors that support it (KDE Plasma, Sway, Hyprland, etc.) will draw their own title bar. If the compositor doesn't support the protocol or requests client-side decorations (e.g. GNOME/Mutter), mdview draws a minimal dark title bar with a close button.

## Runtime dependencies

Only libraries already loaded in any Wayland session:

- `libwayland-client.so`
- `libwayland-cursor.so` (only loaded on compositors lacking `wp_cursor_shape_v1`)
- `libxkbcommon.so`
- `libcairo.so`
- `libpangocairo-1.0.so` / `libpango-1.0.so`
- `libgdk_pixbuf-2.0.so` (image loading)
- `librsvg-2.0.so` (SVG rendering)

## License

- litehtml: BSD-3-Clause
- md4c: MIT
- Cairo, Pango, Wayland: LGPL

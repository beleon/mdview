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

**1,440 lines of our code. 1.3 MB binary. Zero frameworks.**

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

## Building

### Prerequisites

- CMake ≥ 3.16, C++17 compiler
- `libwayland-dev libwayland-cursor0 wayland-protocols libxkbcommon-dev libcairo2-dev libpango1.0-dev`

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Build with Podman

```bash
chmod +x build.sh
./build.sh
```

## Usage

```bash
./mdview path/to/document.md
```

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
| `Alt+C` | Toggle case-sensitive search |
| `Ctrl+Backspace` | Delete word in find bar |
| `Ctrl++` / `Ctrl+=` | Zoom in |
| `Ctrl+-` | Zoom out |
| `Ctrl+0` | Reset zoom |
| `q` / `Escape` | Quit |

### Mouse

- **Scroll wheel** — scroll document
- **Shift+wheel** — horizontal scroll
- **Ctrl+wheel** — zoom in / out
- **Trackpad pinch** — zoom in / out (compositors that support `pointer-gestures-unstable-v1`)
- **Click + drag** — select text
- **Double-click** — select word (drag to extend by words)
- **Triple-click** — select line (drag to extend by lines)
- **Title bar drag** — move window (CSD mode)

## Window Decorations

mdview negotiates server-side decorations via the `xdg-decoration-unstable-v1` protocol. Compositors that support it (KDE Plasma, Sway, Hyprland, etc.) will draw their own title bar. If the compositor doesn't support the protocol or requests client-side decorations (e.g. GNOME/Mutter), mdview draws a minimal dark title bar with a close button.

## Runtime dependencies

Only libraries already loaded in any Wayland session:

- `libwayland-client.so`
- `libxkbcommon.so`
- `libcairo.so`
- `libpangocairo-1.0.so` / `libpango-1.0.so`

## License

- litehtml: BSD-3-Clause
- md4c: MIT
- Cairo, Pango, Wayland: LGPL

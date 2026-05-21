# Screenshots

The repo README references three PNGs in this folder. Until you drop them
in, GitHub will show broken-image placeholders, which is fine while you
iterate, but worth fixing before any link gets shared.

## Required files

| File | Used as | Suggested window |
|---|---|---|
| `hero.png` | Top-of-README banner (rendered at 600 px wide) | Light mode, a longish doc scrolled near the top so headings + paragraphs + a code block are visible at once. ~1000×640 captures well at the 600 px display size. |
| `outline.png` | Showcase grid (left) | 960×640, light mode, outline sidebar (`Ctrl+T`) visible alongside the document |
| `find.png` | Showcase grid (right) | 960×640, dark mode, find bar open with a query that has multiple matches, ideally with one toggle (Aa / Word / .*) active |

Keep the two showcase-grid files at the same aspect ratio (3:2 works
well) so the table stays tidy.

## Capturing on Wayland

There's no single capture tool that works everywhere; pick what your
compositor ships with:

### GNOME
```bash
# Full window, click to choose:
gnome-screenshot -w -f docs/screenshots/hero.png
```

### KDE Plasma
```bash
spectacle -a -b -o docs/screenshots/hero.png
```

### Sway / Hyprland / wlroots compositors
```bash
# `grim` + `slurp` for an interactive rect:
grim -g "$(slurp)" docs/screenshots/hero.png

# Or capture the active window via swaymsg:
grim -g "$(swaymsg -t get_tree | jq -r '.. | select(.focused?) | .rect | "\(.x),\(.y) \(.width)x\(.height)"')" docs/screenshots/hero.png
```

## Test document

For consistent screenshots, use a doc that exercises the engine. The
repo ships [`test.md`](../../test.md) which has headings, paragraphs,
lists, a code block, a table, and a blockquote.

```bash
./dist/mdview test.md
```

## Quality checks

- Crop tight: no desktop background, no dock, no neighbouring windows.
- Use the **default cursor theme** so the screenshot doesn't look
  like your personal setup. (mdview honours `XCURSOR_THEME`, so you can
  override it just for the capture: `XCURSOR_THEME=Adwaita ./mdview …`.)
- PNG, not JPG. Markdown viewers don't have animated content; static
  PNG keeps text crisp.
- Optional: optimise with `oxipng -o 6 docs/screenshots/*.png` to shave
  a few hundred KB. Not required.

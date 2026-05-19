# Hello MDView

This is a **markdown viewer** built with libwayland-client + Cairo + Pango + litehtml + md4c.

## Features

- Converts Markdown to HTML via [md4c](https://github.com/mity/md4c)
- Renders HTML via litehtml
- Draws in a native Wayland window using Cairo + Pango
- Supports scrolling

## Code Example

```c
int main(int argc, char** argv) {
    printf("Hello, world!\n");
    return 0;
}
```

## Table

| Feature    | Status |
|------------|--------|
| Headers    | ✅     |
| Bold/Italic| ✅     |
| Lists      | ✅     |
| Tables     | ✅     |
| Code       | ✅     |

> This is a blockquote. It should be rendered with a left border.

### Ordered List

1. First item
2. Second item
3. Third item

---

*Built with ❤️ using Wayland, Cairo, Pango, litehtml, and md4c*

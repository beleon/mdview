#pragma once

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mdv_window mdv_window;

typedef struct {
    void (*on_redraw)(mdv_window *win, cairo_t *cr, int width, int height, void *userdata);
    void (*on_resize)(mdv_window *win, int width, int height, void *userdata);
    void (*on_mouse_move)(mdv_window *win, int x, int y, void *userdata);
    void (*on_mouse_button)(mdv_window *win, int x, int y, int button, uint32_t modifiers, bool pressed, int click_count, void *userdata);
    void (*on_scroll)(mdv_window *win, int x, int y, double dx, double dy, uint32_t modifiers, void *userdata);
    void (*on_key)(mdv_window *win, uint32_t keysym, uint32_t modifiers, bool pressed, void *userdata);
    void (*on_close)(mdv_window *win, void *userdata);
    void (*on_pointer_leave)(mdv_window *win, int last_x, int last_y, void *userdata);
    void (*on_pointer_enter)(mdv_window *win, int x, int y, void *userdata);
    void (*on_tick)(mdv_window *win, void *userdata);
    void (*on_pinch)(mdv_window *win, double scale, int cx, int cy, bool finished, void *userdata);
    void *userdata;
} mdv_callbacks;

#define MDV_MOD_CTRL  (1 << 0)
#define MDV_MOD_SHIFT (1 << 1)
#define MDV_MOD_ALT   (1 << 2)

mdv_window *mdv_window_create(const char *title, int width, int height, const mdv_callbacks *cb);
void         mdv_window_destroy(mdv_window *win);
void         mdv_window_run(mdv_window *win);
void         mdv_window_request_redraw(mdv_window *win);
int          mdv_window_width(mdv_window *win);
int          mdv_window_height(mdv_window *win);
void         mdv_window_quit(mdv_window *win);
void         mdv_window_copy_to_clipboard(mdv_window *win, const char *text);
void         mdv_window_set_cursor(mdv_window *win, const char *name);
void         mdv_window_set_tick_enabled(mdv_window *win, bool enabled);

#ifdef __cplusplus
}
#endif

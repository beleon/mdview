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
int          mdv_window_width(mdv_window *win);         /* viewport content w */
int          mdv_window_height(mdv_window *win);        /* viewport content h (minus CSD title bar) */
int          mdv_window_surface_width(mdv_window *win); /* full wl_surface w */
int          mdv_window_surface_height(mdv_window *win);/* full wl_surface h (incl. CSD title bar) */
void         mdv_window_quit(mdv_window *win);
void         mdv_window_copy_to_clipboard(mdv_window *win, const char *text);
/* Blocking paste from the system clipboard. Returns malloc'd NUL-terminated
   string (caller frees) or NULL if no text clipboard offer is present. */
char        *mdv_window_paste_blocking(mdv_window *win);
void         mdv_window_set_cursor(mdv_window *win, const char *name);
void         mdv_window_set_tick_enabled(mdv_window *win, bool enabled);

/* Watch an external fd from inside the event loop. cb fires on POLLIN/POLLHUP.
   Up to MDV_MAX_WATCHED_FDS may be registered at once. Returns 0 on success,
   -1 if the fd is already watched or the table is full. */
#define MDV_MAX_WATCHED_FDS 16
typedef void (*mdv_pollfd_cb)(mdv_window *win, int fd, void *userdata);
int  mdv_window_watch_fd(mdv_window *win, int fd, mdv_pollfd_cb cb, void *userdata);
void mdv_window_unwatch_fd(mdv_window *win, int fd);

/* Fire `cb` once, just after the compositor acks the first frame the window
   draws. Useful for kicking off work that shouldn't delay first paint. If the
   first frame has already happened by the time you register, `cb` is invoked
   inline. */
typedef void (*mdv_after_init_cb)(mdv_window *win, void *userdata);
void mdv_window_set_after_init(mdv_window *win, mdv_after_init_cb cb, void *userdata);

#ifdef __cplusplus
}
#endif

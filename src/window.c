#define _GNU_SOURCE
#include "window.h"
#include "xdg-shell-protocol.h"
#include "xdg-decoration-protocol.h"
#include "pointer-gestures-protocol.h"
#include "cursor-shape-protocol.h"

#include <pango/pangocairo.h>
#include <wayland-cursor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <math.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <linux/input-event-codes.h>

#define CSD_TITLEBAR_HEIGHT 32
#define CSD_BUTTON_SIZE     28
#define TICK_MS             30
#define DEFAULT_REPEAT_RATE 25   /* keys/sec, used if compositor doesn't advertise */
#define DEFAULT_REPEAT_DELAY 400 /* ms */
#define MULTI_CLICK_MS      400  /* time window for double/triple click */
#define MULTI_CLICK_DIST_SQ 25   /* 5px radius, squared */

/* Cursor identity. With cursor-shape-v1, the compositor draws the user's
   actual themed cursor; we just say which shape. Without it we fall back to
   wl_cursor_theme and an embedded buffer. */
typedef enum {
    MDV_CURSOR_DEFAULT,
    MDV_CURSOR_TEXT,
    MDV_CURSOR_POINTER,
    MDV_CURSOR_NW_RESIZE,
    MDV_CURSOR_NE_RESIZE,
    MDV_CURSOR_SW_RESIZE,
    MDV_CURSOR_SE_RESIZE,
    MDV_CURSOR_NS_RESIZE,
    MDV_CURSOR_EW_RESIZE,
    MDV_CURSOR_COUNT,
} mdv_cursor_kind;

typedef struct mdv_buffer mdv_buffer;
struct mdv_buffer {
    mdv_window *win;
    struct wl_buffer *wl_buf;
    void *data; int width, height, stride; size_t size;
    bool busy;
    bool dispose_on_release;
    cairo_surface_t *cairo_surface;
    mdv_buffer *next_dispose;
};

struct mdv_window {
    struct wl_display *display; struct wl_registry *registry;
    struct wl_compositor *compositor; struct wl_shm *shm; struct wl_seat *seat;
    struct xdg_wm_base *xdg_wm_base; struct wl_surface *surface;
    struct xdg_surface *xdg_surface; struct xdg_toplevel *xdg_toplevel;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct zxdg_toplevel_decoration_v1 *toplevel_decoration;
    bool use_csd; char *title;
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device; struct wl_data_source *data_source;
    char *clipboard_text;
    struct wl_pointer *pointer; struct wl_keyboard *keyboard;
    struct xkb_context *xkb_ctx; struct xkb_keymap *xkb_keymap; struct xkb_state *xkb_state;
    int mouse_x, mouse_y; uint32_t modifiers, last_serial;
    /* The cursor-shape protocol requires the most recent wl_pointer.enter
       serial specifically; last_serial gets clobbered by button events. */
    uint32_t last_enter_serial;
    /* Preferred cursor path: compositor draws the user's theme via shape-v1.
       Set only if the compositor exposes wp_cursor_shape_manager_v1 AND we
       have a pointer to bind it against. */
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1  *cursor_shape_device;
    /* Fallback: wl_cursor_theme with our own surface. Only loaded if the
       shape protocol isn't available. */
    struct wl_cursor_theme *cursor_theme; struct wl_surface *cursor_surface;
    struct wl_cursor *cursor_table[MDV_CURSOR_COUNT];
    mdv_cursor_kind cursor_current;
    /* Pinch gesture */
    struct zwp_pointer_gestures_v1 *pointer_gestures;
    struct zwp_pointer_gesture_pinch_v1 *pinch_gesture;
    int pinch_cx, pinch_cy;
    /* Frame / buffers (heap-allocated; busy buffers deferred via dispose_list) */
    struct wl_callback *frame_cb;
    mdv_buffer *buffers[2];
    mdv_buffer *dispose_list;
    int current_buf;
    int surf_width, surf_height; bool configured, running, needs_redraw;
    int timerfd; bool tick_enabled;
    /* Key repeat */
    int repeat_timerfd;
    int32_t repeat_rate;   /* keys/sec, 0 = no repeat */
    int32_t repeat_delay;  /* ms */
    uint32_t repeat_key;   /* raw keycode used to match release */
    uint32_t repeat_sym;
    uint32_t repeat_mods;
    bool repeat_armed;
    /* Multi-click detection (BTN_LEFT). Cycles 1→2→3→1 within MULTI_CLICK_MS
       and MULTI_CLICK_DIST. Held across press+release of one click. */
    struct timespec last_click_time;
    int last_click_x, last_click_y;
    int click_count;
    /* External fds watched by the event loop (subprocess output, etc.). */
    struct { int fd; mdv_pollfd_cb cb; void *ud; } watched[MDV_MAX_WATCHED_FDS];
    int n_watched;
    /* Fired once after the first frame is acked by the compositor. */
    bool first_frame_done;
    mdv_after_init_cb after_init_cb;
    void *after_init_ud;
    /* HiDPI: compositor-suggested integer buffer scale (1 = stay logical). */
    int buffer_scale;
    int compositor_version;
    mdv_callbacks cb;
};

static int tb_h(mdv_window *w) { return w->use_csd ? CSD_TITLEBAR_HEIGHT : 0; }
static int cw(mdv_window *w) { return w->surf_width; }
static int ch(mdv_window *w) { return w->surf_height - tb_h(w); }

static int mk_shm(size_t sz) {
    int fd = memfd_create("mdv", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)sz) < 0) { close(fd); return -1; }
    return fd;
}

static void bfree(mdv_buffer *b) {
    if (!b) return;
    if (b->cairo_surface) cairo_surface_destroy(b->cairo_surface);
    if (b->wl_buf) wl_buffer_destroy(b->wl_buf);
    if (b->data && b->data != MAP_FAILED) munmap(b->data, b->size);
    free(b);
}

static void buf_rel(void *d, struct wl_buffer *wb) {
    (void)wb;
    mdv_buffer *b = d;
    b->busy = false;
    if (b->dispose_on_release) {
        /* Unlink from window's dispose_list and free */
        mdv_window *w = b->win;
        mdv_buffer **pp = &w->dispose_list;
        while (*pp) {
            if (*pp == b) { *pp = b->next_dispose; break; }
            pp = &(*pp)->next_dispose;
        }
        bfree(b);
    }
}
static const struct wl_buffer_listener bl = { .release = buf_rel };

/* `ww` and `hh` are logical (surface) dimensions. The actual pixel buffer is
   sized up by w->buffer_scale so that drawing through cairo scaled by
   buffer_scale produces crisp output at HiDPI. */
static mdv_buffer *bnew(mdv_window *w, int ww, int hh) {
    mdv_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    int s = w->buffer_scale > 0 ? w->buffer_scale : 1;
    int pw = ww * s, ph = hh * s;
    b->win = w;
    b->stride = pw * 4; b->width = pw; b->height = ph;
    b->size = (size_t)(b->stride * ph);
    int fd = mk_shm(b->size);
    if (fd < 0) { free(b); return NULL; }
    b->data = mmap(NULL, b->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (b->data == MAP_FAILED) { close(fd); free(b); return NULL; }
    struct wl_shm_pool *p = wl_shm_create_pool(w->shm, fd, (int32_t)b->size);
    b->wl_buf = wl_shm_pool_create_buffer(p, 0, pw, ph, b->stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(p); close(fd);
    wl_buffer_add_listener(b->wl_buf, &bl, b);
    b->cairo_surface = cairo_image_surface_create_for_data((unsigned char*)b->data, CAIRO_FORMAT_ARGB32, pw, ph, b->stride);
    return b;
}

/* Replace both buffers. Busy ones are queued for dispose-on-release; the
   compositor will release them when it's no longer holding the wl_buffer. */
static void rebufs(mdv_window *w, int ww, int hh) {
    for (int i = 0; i < 2; i++) {
        mdv_buffer *old = w->buffers[i];
        if (old) {
            if (old->busy) {
                old->dispose_on_release = true;
                old->next_dispose = w->dispose_list;
                w->dispose_list = old;
            } else {
                bfree(old);
            }
        }
        w->buffers[i] = bnew(w, ww, hh);
    }
}

/* CSD */
static void draw_csd(mdv_window *w, cairo_t *cr) {
    int ww=w->surf_width, h=CSD_TITLEBAR_HEIGHT;
    cairo_set_source_rgb(cr,.22,.22,.24); cairo_rectangle(cr,0,0,ww,h); cairo_fill(cr);
    cairo_set_source_rgb(cr,.15,.15,.16); cairo_rectangle(cr,0,h-1,ww,1); cairo_fill(cr);
    if(w->title){
        PangoLayout *l=pango_cairo_create_layout(cr);
        PangoFontDescription *fd=pango_font_description_from_string("Sans 11");
        pango_layout_set_font_description(l,fd); pango_layout_set_text(l,w->title,-1);
        int tw,th; pango_layout_get_pixel_size(l,&tw,&th);
        cairo_set_source_rgb(cr,.9,.9,.9); cairo_move_to(cr,(ww-tw)/2.0,(h-th)/2.0);
        pango_cairo_show_layout(cr,l); pango_font_description_free(fd); g_object_unref(l);
    }
    double bx=ww-CSD_BUTTON_SIZE-4, by=(h-CSD_BUTTON_SIZE)/2.0;
    double cx=bx+CSD_BUTTON_SIZE/2.0, cy=by+CSD_BUTTON_SIZE/2.0, r=6;
    cairo_set_source_rgb(cr,.85,.25,.25);
    cairo_arc(cr,cx,cy,CSD_BUTTON_SIZE/2.0-2,0,2*M_PI); cairo_fill(cr);
    cairo_set_source_rgb(cr,1,1,1); cairo_set_line_width(cr,1.5);
    cairo_move_to(cr,cx-r+3,cy-r+3); cairo_line_to(cr,cx+r-3,cy+r-3);
    cairo_move_to(cr,cx+r-3,cy-r+3); cairo_line_to(cr,cx-r+3,cy+r-3); cairo_stroke(cr);
}
static bool csd_close(mdv_window *w, int sx, int sy) { return w->use_csd && sy<CSD_TITLEBAR_HEIGHT && sx>=w->surf_width-CSD_BUTTON_SIZE-4; }
static bool csd_tb(mdv_window *w, int sx, int sy) { (void)sx; return w->use_csd && sy<CSD_TITLEBAR_HEIGHT; }

#define CSD_EDGE_W 6
static uint32_t csd_resize_edge(mdv_window *w, int sx, int sy) {
    if (!w->use_csd) return 0;
    int ww = w->surf_width, hh = w->surf_height;
    bool top = sy < CSD_EDGE_W, bot = sy >= hh - CSD_EDGE_W;
    bool left = sx < CSD_EDGE_W, right = sx >= ww - CSD_EDGE_W;
    if (top && left)   return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    if (top && right)  return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    if (bot && left)   return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    if (bot && right)  return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    if (top)           return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    if (bot)           return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    if (left)          return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    if (right)         return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    return 0;
}

static const uint32_t SHAPE_FOR[MDV_CURSOR_COUNT] = {
    [MDV_CURSOR_DEFAULT]   = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
    [MDV_CURSOR_TEXT]      = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,
    [MDV_CURSOR_POINTER]   = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
    [MDV_CURSOR_NW_RESIZE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE,
    [MDV_CURSOR_NE_RESIZE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE,
    [MDV_CURSOR_SW_RESIZE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE,
    [MDV_CURSOR_SE_RESIZE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE,
    [MDV_CURSOR_NS_RESIZE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
    [MDV_CURSOR_EW_RESIZE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
};

/* Draw the cursor for `kind` right now. Preferred path delegates to the
   compositor via wp_cursor_shape_device_v1, which means the user's actual
   cursor theme is honoured (this is what every other Wayland app does on
   modern compositors — Hyprland/KDE/Sway/etc). Fallback path loads
   wl_cursor_theme ourselves and attaches an SHM-backed cursor surface. */
static void apply_cursor_kind(mdv_window *w, mdv_cursor_kind kind) {
    if (!w->pointer) return;
    if (w->cursor_shape_device) {
        wp_cursor_shape_device_v1_set_shape(w->cursor_shape_device,
                                            w->last_enter_serial,
                                            SHAPE_FOR[kind]);
        return;
    }
    if (!w->cursor_surface) return;
    struct wl_cursor *c = w->cursor_table[kind];
    if (!c) c = w->cursor_table[MDV_CURSOR_DEFAULT];
    if (!c) return;
    struct wl_cursor_image *i = c->images[0];
    struct wl_buffer *b = wl_cursor_image_get_buffer(i);
    if (!b) return;
    wl_surface_attach(w->cursor_surface, b, 0, 0);
    wl_surface_damage(w->cursor_surface, 0, 0, (int)i->width, (int)i->height);
    wl_surface_commit(w->cursor_surface);
    wl_pointer_set_cursor(w->pointer, w->last_enter_serial, w->cursor_surface,
                          (int)i->hotspot_x, (int)i->hotspot_y);
}

/* Switch to a different cursor kind. No-op if it's already current — the
   shape protocol allows redundant set_shape but skipping cuts traffic. */
static void set_cursor_kind(mdv_window *w, mdv_cursor_kind kind) {
    if (kind == w->cursor_current) return;
    w->cursor_current = kind;
    apply_cursor_kind(w, kind);
}

/* XDG */
static void xwp(void *d,struct xdg_wm_base *b,uint32_t s){(void)d;xdg_wm_base_pong(b,s);}
static const struct xdg_wm_base_listener xwl={.ping=xwp};
static void xsc(void *d,struct xdg_surface *s,uint32_t sr){mdv_window *w=d;xdg_surface_ack_configure(s,sr);w->configured=true;w->needs_redraw=true;}
static const struct xdg_surface_listener xsl={.configure=xsc};
static void xtc(void *d,struct xdg_toplevel *t,int32_t ww,int32_t hh,struct wl_array *st){
    (void)t;(void)st;mdv_window *w=d;
    if(ww>0&&hh>0&&(ww!=w->surf_width||hh!=w->surf_height)){
        w->surf_width=ww;w->surf_height=hh;rebufs(w,ww,hh);
        if(w->cb.on_resize)w->cb.on_resize(w,cw(w),ch(w),w->cb.userdata);
    }
}
static void xtcl(void *d,struct xdg_toplevel *t){(void)t;mdv_window *w=d;if(w->cb.on_close)w->cb.on_close(w,w->cb.userdata);w->running=false;}
static const struct xdg_toplevel_listener xtl={.configure=xtc,.close=xtcl};
static void dc(void *d,struct zxdg_toplevel_decoration_v1 *x,uint32_t m){(void)x;((mdv_window*)d)->use_csd=(m==ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);}
static const struct zxdg_toplevel_decoration_v1_listener dl={.configure=dc};

/* Clipboard */
static void dst(void *d,struct wl_data_source *s,const char *m){(void)d;(void)s;(void)m;}
static void dss(void *d,struct wl_data_source *s,const char *m,int32_t fd){
    (void)s;(void)m;mdv_window *w=d;
    if(w->clipboard_text){size_t l=strlen(w->clipboard_text),o=0;while(o<l){ssize_t n=write(fd,w->clipboard_text+o,l-o);if(n<=0)break;o+=(size_t)n;}}
    close(fd);
}
static void dsc(void *d,struct wl_data_source *s){mdv_window *w=d;if(w->data_source==s)w->data_source=NULL;wl_data_source_destroy(s);}
static const struct wl_data_source_listener dsl={.target=dst,.send=dss,.cancelled=dsc};

/* Key repeat */
static void disarm_repeat(mdv_window *w) {
    if (!w->repeat_armed) return;
    w->repeat_armed = false;
    if (w->repeat_timerfd >= 0) {
        struct itimerspec its = {0};
        timerfd_settime(w->repeat_timerfd, 0, &its, NULL);
    }
}
static void arm_repeat(mdv_window *w, uint32_t key, uint32_t sym, uint32_t mods) {
    if (w->repeat_timerfd < 0 || w->repeat_rate <= 0) return;
    w->repeat_key = key; w->repeat_sym = sym; w->repeat_mods = mods;
    w->repeat_armed = true;
    struct itimerspec its = {0};
    its.it_value.tv_sec  = w->repeat_delay / 1000;
    its.it_value.tv_nsec = (w->repeat_delay % 1000) * 1000000L;
    long interval_ns = 1000000000L / w->repeat_rate;
    its.it_interval.tv_sec  = interval_ns / 1000000000L;
    its.it_interval.tv_nsec = interval_ns % 1000000000L;
    timerfd_settime(w->repeat_timerfd, 0, &its, NULL);
}

/* Returns the (new) click_count for the current BTN_LEFT press. Cycles
   1→2→3→1 if the press is within MULTI_CLICK_MS of the previous one and
   within MULTI_CLICK_DIST_SQ pixels squared. Otherwise resets to 1. */
static int update_click_count(mdv_window *w) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - w->last_click_time.tv_sec) * 1000L
                    + (now.tv_nsec - w->last_click_time.tv_nsec) / 1000000L;
    int dx = w->mouse_x - w->last_click_x;
    int dy = w->mouse_y - w->last_click_y;
    bool same_spot = (dx*dx + dy*dy) <= MULTI_CLICK_DIST_SQ;
    bool quick     = elapsed_ms >= 0 && elapsed_ms < MULTI_CLICK_MS;
    if (quick && same_spot) w->click_count = (w->click_count >= 3) ? 1 : w->click_count + 1;
    else                    w->click_count = 1;
    w->last_click_time = now;
    w->last_click_x = w->mouse_x;
    w->last_click_y = w->mouse_y;
    return w->click_count;
}

/* Pointer */
static void pe(void *d,struct wl_pointer *p,uint32_t sr,struct wl_surface *sf,wl_fixed_t sx,wl_fixed_t sy){
    (void)p;(void)sf;mdv_window *w=d;w->mouse_x=wl_fixed_to_int(sx);w->mouse_y=wl_fixed_to_int(sy);
    w->last_serial=sr;w->last_enter_serial=sr;
    apply_cursor_kind(w,w->cursor_current);
    int tb=tb_h(w);if(w->cb.on_pointer_enter&&w->mouse_y>=tb)w->cb.on_pointer_enter(w,w->mouse_x,w->mouse_y-tb,w->cb.userdata);
}
static void pl(void *d,struct wl_pointer *p,uint32_t sr,struct wl_surface *sf){
    (void)p;(void)sr;(void)sf;mdv_window *w=d;int tb=tb_h(w);
    /* Clamp to 0 if pointer was over the title bar — we never sent the matching
       enter for that region, but downstream still expects non-negative coords. */
    int cy = w->mouse_y - tb;
    if (cy < 0) cy = 0;
    if (w->cb.on_pointer_leave) w->cb.on_pointer_leave(w, w->mouse_x, cy, w->cb.userdata);
}
static void pm(void *d,struct wl_pointer *p,uint32_t t,wl_fixed_t sx,wl_fixed_t sy){
    (void)p;(void)t;mdv_window *w=d;w->mouse_x=wl_fixed_to_int(sx);w->mouse_y=wl_fixed_to_int(sy);
    if(w->use_csd){
        uint32_t edge=csd_resize_edge(w,w->mouse_x,w->mouse_y);
        mdv_cursor_kind ek = MDV_CURSOR_DEFAULT;
        bool over_edge = true;
        switch(edge){
        case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:     ek=MDV_CURSOR_NW_RESIZE; break;
        case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:    ek=MDV_CURSOR_NE_RESIZE; break;
        case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:  ek=MDV_CURSOR_SW_RESIZE; break;
        case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT: ek=MDV_CURSOR_SE_RESIZE; break;
        case XDG_TOPLEVEL_RESIZE_EDGE_TOP: case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM: ek=MDV_CURSOR_NS_RESIZE; break;
        case XDG_TOPLEVEL_RESIZE_EDGE_LEFT: case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT: ek=MDV_CURSOR_EW_RESIZE; break;
        default: over_edge = false; break;
        }
        if(over_edge){ set_cursor_kind(w, ek); return; }
        if(w->cursor_current >= MDV_CURSOR_NW_RESIZE
        && w->cursor_current <= MDV_CURSOR_EW_RESIZE){
            set_cursor_kind(w, MDV_CURSOR_DEFAULT);
        }
    }
    /* Always forward motion — including when the pointer is in the CSD title
       bar or has been dragged out of the surface entirely (which still
       generates motion events while a button is held, due to the implicit
       grab). The app needs those out-of-viewport coordinates to drive
       autoscroll during a selection drag. */
    int tb=tb_h(w);if(w->cb.on_mouse_move)w->cb.on_mouse_move(w,w->mouse_x,w->mouse_y-tb,w->cb.userdata);
}
static void pb(void *d,struct wl_pointer *p,uint32_t sr,uint32_t t,uint32_t btn,uint32_t st){
    (void)p;(void)t;mdv_window *w=d;w->last_serial=sr;bool pr=(st==WL_POINTER_BUTTON_STATE_PRESSED);
    if(w->use_csd&&pr&&btn==BTN_LEFT){
        if(csd_close(w,w->mouse_x,w->mouse_y)){if(w->cb.on_close)w->cb.on_close(w,w->cb.userdata);w->running=false;return;}
        uint32_t edge=csd_resize_edge(w,w->mouse_x,w->mouse_y);
        if(edge){xdg_toplevel_resize(w->xdg_toplevel,w->seat,sr,edge);return;}
        if(csd_tb(w,w->mouse_x,w->mouse_y)){xdg_toplevel_move(w->xdg_toplevel,w->seat,sr);return;}
    }
    int count = w->click_count;
    if (pr && btn == BTN_LEFT) count = update_click_count(w);
    int tb=tb_h(w);
    if(pr){if(w->mouse_y>=tb&&w->cb.on_mouse_button)w->cb.on_mouse_button(w,w->mouse_x,w->mouse_y-tb,(int)btn,w->modifiers,pr,count,w->cb.userdata);}
    else{if(w->cb.on_mouse_button)w->cb.on_mouse_button(w,w->mouse_x,(w->mouse_y-tb>0?w->mouse_y-tb:0),(int)btn,w->modifiers,pr,count,w->cb.userdata);}
}
static void pa(void *d,struct wl_pointer *p,uint32_t t,uint32_t ax,wl_fixed_t val){
    (void)p;(void)t;mdv_window *w=d;int tb=tb_h(w);
    if(w->mouse_y>=tb&&w->cb.on_scroll){
        double dx=(ax==WL_POINTER_AXIS_HORIZONTAL_SCROLL)?wl_fixed_to_double(val):0;
        double dy=(ax==WL_POINTER_AXIS_VERTICAL_SCROLL)?wl_fixed_to_double(val):0;
        w->cb.on_scroll(w,w->mouse_x,w->mouse_y-tb,dx,dy,w->modifiers,w->cb.userdata);
    }
}
static void pf(void *d,struct wl_pointer *p){(void)d;(void)p;}
static void pas(void *d,struct wl_pointer *p,uint32_t s){(void)d;(void)p;(void)s;}
static void past(void *d,struct wl_pointer *p,uint32_t t,uint32_t a){(void)d;(void)p;(void)t;(void)a;}
static void pad(void *d,struct wl_pointer *p,uint32_t a,int32_t v){(void)d;(void)p;(void)a;(void)v;}
static const struct wl_pointer_listener ptl={.enter=pe,.leave=pl,.motion=pm,.button=pb,.axis=pa,.frame=pf,.axis_source=pas,.axis_stop=past,.axis_discrete=pad};

/* Pinch gesture */
static void pinch_begin(void *d, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t sr, uint32_t t, struct wl_surface *sf, uint32_t fingers) {
    (void)g;(void)sr;(void)t;(void)sf;(void)fingers;
    mdv_window *w = d;
    w->pinch_cx = w->mouse_x;
    w->pinch_cy = w->mouse_y - tb_h(w);
    if (w->cb.on_pinch)
        w->cb.on_pinch(w, 1.0, w->pinch_cx, w->pinch_cy, false, w->cb.userdata);
}
static void pinch_update(void *d, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rot) {
    (void)g;(void)t;(void)dx;(void)dy;(void)rot;
    mdv_window *w = d;
    double s = wl_fixed_to_double(scale);
    if (w->cb.on_pinch)
        w->cb.on_pinch(w, s, w->pinch_cx, w->pinch_cy, false, w->cb.userdata);
}
static void pinch_end(void *d, struct zwp_pointer_gesture_pinch_v1 *g, uint32_t sr, uint32_t t, int32_t cancelled) {
    (void)g;(void)sr;(void)t;(void)cancelled;
    mdv_window *w = d;
    if (w->cb.on_pinch)
        w->cb.on_pinch(w, 1.0, w->pinch_cx, w->pinch_cy, true, w->cb.userdata);
}
static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
    .begin = pinch_begin, .update = pinch_update, .end = pinch_end
};

/* Keyboard */
static void kk(void *d,struct wl_keyboard *k,uint32_t f,int32_t fd,uint32_t sz){
    (void)k;mdv_window *w=d;if(f!=WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1){close(fd);return;}
    char *m=mmap(NULL,sz,PROT_READ,MAP_PRIVATE,fd,0);close(fd);if(m==MAP_FAILED)return;
    if(w->xkb_keymap)xkb_keymap_unref(w->xkb_keymap);if(w->xkb_state)xkb_state_unref(w->xkb_state);
    w->xkb_keymap=xkb_keymap_new_from_string(w->xkb_ctx,m,XKB_KEYMAP_FORMAT_TEXT_V1,XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(m,sz);if(w->xkb_keymap)w->xkb_state=xkb_state_new(w->xkb_keymap);
}
static void ke(void *d,struct wl_keyboard *k,uint32_t s,struct wl_surface *sf,struct wl_array *keys){(void)d;(void)k;(void)s;(void)sf;(void)keys;}
static void kl(void *d,struct wl_keyboard *k,uint32_t s,struct wl_surface *sf){
    (void)k;(void)s;(void)sf; mdv_window *w=d; disarm_repeat(w);
}
static void um(mdv_window *w){
    w->modifiers=0;
    if(xkb_state_mod_name_is_active(w->xkb_state,XKB_MOD_NAME_CTRL,XKB_STATE_MODS_EFFECTIVE))w->modifiers|=MDV_MOD_CTRL;
    if(xkb_state_mod_name_is_active(w->xkb_state,XKB_MOD_NAME_SHIFT,XKB_STATE_MODS_EFFECTIVE))w->modifiers|=MDV_MOD_SHIFT;
    if(xkb_state_mod_name_is_active(w->xkb_state,XKB_MOD_NAME_ALT,XKB_STATE_MODS_EFFECTIVE))w->modifiers|=MDV_MOD_ALT;
}
static void ky(void *d,struct wl_keyboard *k,uint32_t sr,uint32_t t,uint32_t key,uint32_t st){
    (void)k;(void)t;mdv_window *w=d;w->last_serial=sr;if(!w->xkb_state)return;
    xkb_keysym_t sym=xkb_state_key_get_one_sym(w->xkb_state,key+8);
    bool pressed = (st==WL_KEYBOARD_KEY_STATE_PRESSED);
    if(w->cb.on_key)w->cb.on_key(w,sym,w->modifiers,pressed,w->cb.userdata);
    if (pressed) arm_repeat(w, key, sym, w->modifiers);
    else if (w->repeat_armed && w->repeat_key == key) disarm_repeat(w);
}
static void km(void *d,struct wl_keyboard *k,uint32_t sr,uint32_t md,uint32_t ml,uint32_t mlk,uint32_t grp){
    (void)k;(void)sr;mdv_window *w=d;if(w->xkb_state){xkb_state_update_mask(w->xkb_state,md,ml,mlk,0,0,grp);um(w);}
}
static void kr(void *d,struct wl_keyboard *k,int32_t r,int32_t dl){
    (void)k; mdv_window *w=d; w->repeat_rate=r; w->repeat_delay=dl;
}
static const struct wl_keyboard_listener kbl={.keymap=kk,.enter=ke,.leave=kl,.key=ky,.modifiers=km,.repeat_info=kr};

/* Seat */
static void sc(void *d,struct wl_seat *s,uint32_t c){
    mdv_window *w=d;
    if((c&WL_SEAT_CAPABILITY_POINTER)&&!w->pointer){
        w->pointer=wl_seat_get_pointer(s);wl_pointer_add_listener(w->pointer,&ptl,w);
        if(w->pointer_gestures){
            w->pinch_gesture=zwp_pointer_gestures_v1_get_pinch_gesture(w->pointer_gestures,w->pointer);
            zwp_pointer_gesture_pinch_v1_add_listener(w->pinch_gesture,&pinch_listener,w);
        }
        if(w->cursor_shape_manager){
            w->cursor_shape_device=wp_cursor_shape_manager_v1_get_pointer(w->cursor_shape_manager,w->pointer);
        }
    }
    if((c&WL_SEAT_CAPABILITY_KEYBOARD)&&!w->keyboard){w->keyboard=wl_seat_get_keyboard(s);wl_keyboard_add_listener(w->keyboard,&kbl,w);}
}
static void sn(void *d,struct wl_seat *s,const char *n){(void)d;(void)s;(void)n;}
static const struct wl_seat_listener stl={.capabilities=sc,.name=sn};

/* wl_surface events. preferred_buffer_scale is the compositor telling us
   what integer scale to render at; we resize the buffer pool and re-render.
   The enter/leave events (for tracking which output we're on) are unused
   because preferred_buffer_scale gives a unified answer. */
static void surface_enter(void *d, struct wl_surface *s, struct wl_output *o) { (void)d;(void)s;(void)o; }
static void surface_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d;(void)s;(void)o; }
static void surface_preferred_buffer_scale(void *d, struct wl_surface *s, int32_t factor) {
    (void)s;
    mdv_window *w = d;
    if (factor < 1) factor = 1;
    if (factor == w->buffer_scale) return;
    w->buffer_scale = factor;
    if (w->surf_width > 0 && w->surf_height > 0) {
        rebufs(w, w->surf_width, w->surf_height);
        w->needs_redraw = true;
    }
}
static void surface_preferred_buffer_transform(void *d, struct wl_surface *s, uint32_t t) { (void)d;(void)s;(void)t; }
static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_buffer_scale,
    .preferred_buffer_transform = surface_preferred_buffer_transform,
};

/* Registry */
static void rg(void *d,struct wl_registry *r,uint32_t name,const char *iface,uint32_t ver){
    (void)ver;mdv_window *w=d;
    if(!strcmp(iface,wl_compositor_interface.name)){
        /* Need v6 for wl_surface.preferred_buffer_scale (HiDPI). Fall back to
           whatever the compositor offers if it's older. */
        uint32_t v = ver >= 6 ? 6 : ver;
        w->compositor=wl_registry_bind(r,name,&wl_compositor_interface,v);
        w->compositor_version = (int)v;
    }
    else if(!strcmp(iface,wl_shm_interface.name))w->shm=wl_registry_bind(r,name,&wl_shm_interface,1);
    else if(!strcmp(iface,wl_seat_interface.name)){w->seat=wl_registry_bind(r,name,&wl_seat_interface,5);wl_seat_add_listener(w->seat,&stl,w);}
    else if(!strcmp(iface,xdg_wm_base_interface.name)){w->xdg_wm_base=wl_registry_bind(r,name,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(w->xdg_wm_base,&xwl,w);}
    else if(!strcmp(iface,zxdg_decoration_manager_v1_interface.name))w->decoration_manager=wl_registry_bind(r,name,&zxdg_decoration_manager_v1_interface,1);
    else if(!strcmp(iface,wl_data_device_manager_interface.name))w->data_device_manager=wl_registry_bind(r,name,&wl_data_device_manager_interface,3);
    else if(!strcmp(iface,zwp_pointer_gestures_v1_interface.name))w->pointer_gestures=wl_registry_bind(r,name,&zwp_pointer_gestures_v1_interface,1);
    else if(!strcmp(iface,wp_cursor_shape_manager_v1_interface.name))w->cursor_shape_manager=wl_registry_bind(r,name,&wp_cursor_shape_manager_v1_interface,1);
}
static void rr(void *d,struct wl_registry *r,uint32_t n){(void)d;(void)r;(void)n;}
static const struct wl_registry_listener rl={.global=rg,.global_remove=rr};

/* Frame */
static void fd_done(void *d,struct wl_callback *cb,uint32_t t);
static const struct wl_callback_listener fl={.done=fd_done};
static void rf(mdv_window *w){
    if(!w->configured)return;
    mdv_buffer *b=w->buffers[w->current_buf];
    if(!b)return;
    if(b->busy){w->current_buf^=1;b=w->buffers[w->current_buf];}
    if(!b||!b->cairo_surface||!b->data)return;
    int s = w->buffer_scale > 0 ? w->buffer_scale : 1;
    cairo_t *cr=cairo_create(b->cairo_surface);
    /* Scale up cairo's coordinate system so logical-pixel drawing fills the
       larger physical buffer crisply. The compositor downscales via
       wl_surface.set_buffer_scale, set just before commit below. */
    if (s != 1) cairo_scale(cr, s, s);
    if(w->use_csd){draw_csd(w,cr);cairo_save(cr);cairo_translate(cr,0,CSD_TITLEBAR_HEIGHT);cairo_rectangle(cr,0,0,cw(w),ch(w));cairo_clip(cr);}
    if(w->cb.on_redraw)w->cb.on_redraw(w,cr,cw(w),ch(w),w->cb.userdata);
    if(w->use_csd)cairo_restore(cr);
    cairo_destroy(cr);cairo_surface_flush(b->cairo_surface);
    wl_surface_attach(w->surface,b->wl_buf,0,0);
    wl_surface_set_buffer_scale(w->surface, s);
    wl_surface_damage_buffer(w->surface,0,0,w->surf_width*s,w->surf_height*s);
    w->frame_cb=wl_surface_frame(w->surface);wl_callback_add_listener(w->frame_cb,&fl,w);
    wl_surface_commit(w->surface);b->busy=true;w->needs_redraw=false;
}
static void fd_done(void *d,struct wl_callback *cb,uint32_t t){
    (void)t;mdv_window *w=d;wl_callback_destroy(cb);w->frame_cb=NULL;
    if (!w->first_frame_done) {
        w->first_frame_done = true;
        if (w->after_init_cb) w->after_init_cb(w, w->after_init_ud);
    }
    if(w->needs_redraw)rf(w);
}

/* Public API */
mdv_window *mdv_window_create(const char *title,int width,int height,const mdv_callbacks *cb){
    mdv_window *w=calloc(1,sizeof(mdv_window));if(!w)return NULL;
    w->surf_width=width;w->surf_height=height;w->cb=*cb;w->title=strdup(title?title:"mdview");
    w->timerfd=-1; w->repeat_timerfd=-1;
    w->repeat_rate=DEFAULT_REPEAT_RATE; w->repeat_delay=DEFAULT_REPEAT_DELAY;
    w->buffer_scale = 1;
    w->xkb_ctx=xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    w->display=wl_display_connect(NULL);
    if(!w->display){fprintf(stderr,"Cannot connect to Wayland\n");free(w->title);free(w);return NULL;}
    w->registry=wl_display_get_registry(w->display);
    wl_registry_add_listener(w->registry,&rl,w);wl_display_roundtrip(w->display);wl_display_roundtrip(w->display);
    if(!w->compositor||!w->shm||!w->xdg_wm_base){fprintf(stderr,"Missing Wayland globals\n");mdv_window_destroy(w);return NULL;}
    w->surface=wl_compositor_create_surface(w->compositor);
    /* Attach surface listener for preferred_buffer_scale (HiDPI). Only fires
       when the compositor binds wl_compositor at v6 or higher. */
    if (w->compositor_version >= 6)
        wl_surface_add_listener(w->surface, &surface_listener, w);
    w->xdg_surface=xdg_wm_base_get_xdg_surface(w->xdg_wm_base,w->surface);
    xdg_surface_add_listener(w->xdg_surface,&xsl,w);
    w->xdg_toplevel=xdg_surface_get_toplevel(w->xdg_surface);
    xdg_toplevel_add_listener(w->xdg_toplevel,&xtl,w);
    xdg_toplevel_set_title(w->xdg_toplevel,w->title);xdg_toplevel_set_app_id(w->xdg_toplevel,"mdview");
    if(w->decoration_manager){
        w->toplevel_decoration=zxdg_decoration_manager_v1_get_toplevel_decoration(w->decoration_manager,w->xdg_toplevel);
        zxdg_toplevel_decoration_v1_add_listener(w->toplevel_decoration,&dl,w);
        zxdg_toplevel_decoration_v1_set_mode(w->toplevel_decoration,ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }else w->use_csd=true;
    if(w->data_device_manager&&w->seat)w->data_device=wl_data_device_manager_get_data_device(w->data_device_manager,w->seat);
    /* Only load the wl_cursor_theme path if the compositor doesn't expose
       cursor-shape-v1 — when it does, the compositor draws cursors itself
       using the user's actual theme. Loading our own theme would just be
       a wasted-memory fallback we'd never use. */
    if (!w->cursor_shape_manager) {
        const char *theme_name = getenv("XCURSOR_THEME");
        int cursor_size = 24;
        const char *size_env = getenv("XCURSOR_SIZE");
        if (size_env) { int sz = atoi(size_env); if (sz > 0) cursor_size = sz; }
        w->cursor_theme = wl_cursor_theme_load(theme_name, cursor_size, w->shm);
        if (w->cursor_theme) {
            w->cursor_surface = wl_compositor_create_surface(w->compositor);
            struct wl_cursor_theme *t = w->cursor_theme;
            #define LOAD(slot, ...) do { \
                const char *names[] = { __VA_ARGS__, NULL }; \
                for (const char **n = names; *n; n++) { \
                    w->cursor_table[slot] = wl_cursor_theme_get_cursor(t, *n); \
                    if (w->cursor_table[slot]) break; \
                } \
            } while (0)
            LOAD(MDV_CURSOR_DEFAULT,   "default", "left_ptr");
            LOAD(MDV_CURSOR_TEXT,      "text", "xterm", "ibeam");
            LOAD(MDV_CURSOR_POINTER,   "pointer", "hand1", "hand2");
            LOAD(MDV_CURSOR_NW_RESIZE, "nw_corner", "top_left_corner", "nw-resize");
            LOAD(MDV_CURSOR_NE_RESIZE, "ne_corner", "top_right_corner", "ne-resize");
            LOAD(MDV_CURSOR_SW_RESIZE, "sw_corner", "bottom_left_corner", "sw-resize");
            LOAD(MDV_CURSOR_SE_RESIZE, "se_corner", "bottom_right_corner", "se-resize");
            LOAD(MDV_CURSOR_NS_RESIZE, "ns-resize", "sb_v_double_arrow");
            LOAD(MDV_CURSOR_EW_RESIZE, "ew-resize", "sb_h_double_arrow");
            #undef LOAD
        }
    }
    w->cursor_current = MDV_CURSOR_DEFAULT;
    for(int i=0;i<2;i++)w->buffers[i]=bnew(w,width,height);
    wl_surface_commit(w->surface);wl_display_roundtrip(w->display);
    w->timerfd=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK);
    w->repeat_timerfd=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK);
    return w;
}
void mdv_window_destroy(mdv_window *w){
    if(!w)return;
    for(int i=0;i<2;i++)bfree(w->buffers[i]);
    /* Drain dispose list (may include buffers still busy from compositor's
       perspective — at this point we're tearing down the connection anyway). */
    mdv_buffer *dp=w->dispose_list;
    while(dp){ mdv_buffer *next=dp->next_dispose; bfree(dp); dp=next; }
    if(w->timerfd>=0)close(w->timerfd);
    if(w->repeat_timerfd>=0)close(w->repeat_timerfd);
    if(w->frame_cb)wl_callback_destroy(w->frame_cb);
    if(w->data_source)wl_data_source_destroy(w->data_source);
    if(w->data_device)wl_data_device_destroy(w->data_device);
    if(w->data_device_manager)wl_data_device_manager_destroy(w->data_device_manager);
    if(w->pinch_gesture)zwp_pointer_gesture_pinch_v1_destroy(w->pinch_gesture);
    if(w->pointer_gestures)zwp_pointer_gestures_v1_release(w->pointer_gestures);
    if(w->cursor_shape_device)wp_cursor_shape_device_v1_destroy(w->cursor_shape_device);
    if(w->cursor_shape_manager)wp_cursor_shape_manager_v1_destroy(w->cursor_shape_manager);
    if(w->cursor_surface)wl_surface_destroy(w->cursor_surface);
    if(w->cursor_theme)wl_cursor_theme_destroy(w->cursor_theme);
    if(w->toplevel_decoration)zxdg_toplevel_decoration_v1_destroy(w->toplevel_decoration);
    if(w->pointer)wl_pointer_destroy(w->pointer);
    if(w->keyboard)wl_keyboard_destroy(w->keyboard);
    if(w->xdg_toplevel)xdg_toplevel_destroy(w->xdg_toplevel);
    if(w->xdg_surface)xdg_surface_destroy(w->xdg_surface);
    if(w->surface)wl_surface_destroy(w->surface);
    if(w->decoration_manager)zxdg_decoration_manager_v1_destroy(w->decoration_manager);
    if(w->xdg_wm_base)xdg_wm_base_destroy(w->xdg_wm_base);
    if(w->seat)wl_seat_destroy(w->seat);if(w->shm)wl_shm_destroy(w->shm);
    if(w->compositor)wl_compositor_destroy(w->compositor);
    if(w->registry)wl_registry_destroy(w->registry);
    if(w->display)wl_display_disconnect(w->display);
    if(w->xkb_state)xkb_state_unref(w->xkb_state);
    if(w->xkb_keymap)xkb_keymap_unref(w->xkb_keymap);
    if(w->xkb_ctx)xkb_context_unref(w->xkb_ctx);
    free(w->title);free(w->clipboard_text);free(w);
}
void mdv_window_run(mdv_window *w){
    w->running=true;rf(w);
    int wl_fd=wl_display_get_fd(w->display);
    struct pollfd fds[3 + MDV_MAX_WATCHED_FDS];
    fds[0].fd=wl_fd;             fds[0].events=POLLIN;
    fds[1].fd=w->timerfd;        fds[1].events=POLLIN;
    fds[2].fd=w->repeat_timerfd; fds[2].events=POLLIN;
    while(w->running){
        wl_display_flush(w->display);
        /* Rebuild the watched-fd portion each iteration; the set can change
           mid-frame as subprocesses start and finish. */
        int nfds=3;
        for(int i=0;i<w->n_watched;i++){
            fds[nfds].fd=w->watched[i].fd;
            fds[nfds].events=POLLIN;
            fds[nfds].revents=0;
            nfds++;
        }
        int ret=poll(fds,nfds,w->tick_enabled?TICK_MS:-1);
        if(ret<0){if(errno==EINTR)continue;break;}
        if(fds[0].revents&POLLIN){if(wl_display_dispatch(w->display)<0)break;}
        else wl_display_dispatch_pending(w->display);
        if(w->tick_enabled){
            if(w->timerfd>=0&&(fds[1].revents&POLLIN)){uint64_t x;ssize_t r=read(w->timerfd,&x,sizeof(x));(void)r;}
            if(w->cb.on_tick)w->cb.on_tick(w,w->cb.userdata);
        }
        if(w->repeat_timerfd>=0&&(fds[2].revents&POLLIN)){
            uint64_t x; ssize_t r=read(w->repeat_timerfd,&x,sizeof(x)); (void)r;
            if(w->repeat_armed&&w->cb.on_key)
                w->cb.on_key(w,w->repeat_sym,w->repeat_mods,true,w->cb.userdata);
        }
        /* Snapshot watched fds before dispatch — a callback may unwatch its
           own fd (and shrink the array), so we must work off a stable copy. */
        struct { int fd; mdv_pollfd_cb cb; void *ud; } snap[MDV_MAX_WATCHED_FDS];
        int snap_n = w->n_watched;
        for (int i = 0; i < snap_n; i++) {
            snap[i].fd = w->watched[i].fd;
            snap[i].cb = w->watched[i].cb;
            snap[i].ud = w->watched[i].ud;
        }
        for (int i = 0; i < snap_n; i++) {
            if (fds[3 + i].revents & (POLLIN | POLLHUP | POLLERR))
                snap[i].cb(w, snap[i].fd, snap[i].ud);
        }
        if(w->needs_redraw&&!w->frame_cb)rf(w);
    }
}
void mdv_window_request_redraw(mdv_window *w){w->needs_redraw=true;}
int mdv_window_width(mdv_window *w){return cw(w);}
int mdv_window_height(mdv_window *w){return ch(w);}
int mdv_window_surface_width(mdv_window *w){return w->surf_width;}
int mdv_window_surface_height(mdv_window *w){return w->surf_height;}
void mdv_window_quit(mdv_window *w){w->running=false;}
void mdv_window_set_tick_enabled(mdv_window *w,bool en){
    w->tick_enabled=en;
    if(w->timerfd>=0){struct itimerspec its={0};if(en){its.it_value.tv_nsec=TICK_MS*1000000L;its.it_interval.tv_nsec=TICK_MS*1000000L;}timerfd_settime(w->timerfd,0,&its,NULL);}
}
void mdv_window_copy_to_clipboard(mdv_window *w,const char *text){
    if(!w->data_device_manager||!w->data_device||!text||!*text)return;
    if(w->data_source){wl_data_source_destroy(w->data_source);w->data_source=NULL;}
    free(w->clipboard_text);w->clipboard_text=strdup(text);
    w->data_source=wl_data_device_manager_create_data_source(w->data_device_manager);
    wl_data_source_add_listener(w->data_source,&dsl,w);
    wl_data_source_offer(w->data_source,"text/plain;charset=utf-8");
    wl_data_source_offer(w->data_source,"text/plain");
    wl_data_source_offer(w->data_source,"UTF8_STRING");
    wl_data_source_offer(w->data_source,"STRING");
    wl_data_device_set_selection(w->data_device,w->data_source,w->last_serial);
}
void mdv_window_set_cursor(mdv_window *w,const char *name){
    if (!w) return;
    mdv_cursor_kind k = MDV_CURSOR_DEFAULT;
    if      (!strcmp(name, "text"))    k = MDV_CURSOR_TEXT;
    else if (!strcmp(name, "pointer")) k = MDV_CURSOR_POINTER;
    set_cursor_kind(w, k);
}
int mdv_window_watch_fd(mdv_window *w, int fd, mdv_pollfd_cb cb, void *ud) {
    if (!w || fd < 0 || !cb) return -1;
    if (w->n_watched >= MDV_MAX_WATCHED_FDS) return -1;
    for (int i = 0; i < w->n_watched; i++) if (w->watched[i].fd == fd) return -1;
    w->watched[w->n_watched].fd = fd;
    w->watched[w->n_watched].cb = cb;
    w->watched[w->n_watched].ud = ud;
    w->n_watched++;
    return 0;
}
void mdv_window_unwatch_fd(mdv_window *w, int fd) {
    if (!w) return;
    for (int i = 0; i < w->n_watched; i++) {
        if (w->watched[i].fd == fd) {
            w->watched[i] = w->watched[--w->n_watched];
            return;
        }
    }
}
void mdv_window_set_after_init(mdv_window *w, mdv_after_init_cb cb, void *ud) {
    if (!w) return;
    w->after_init_cb = cb;
    w->after_init_ud = ud;
    if (w->first_frame_done && cb) cb(w, ud);
}

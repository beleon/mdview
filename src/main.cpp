#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <vector>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <linux/input-event-codes.h>

#include "litehtml.h"
#include "cairo_container.h"
extern "C" {
#include "md4c.h"
#include "md4c-html.h"
#include "window.h"
}

static std::string md2html(const std::string &md) {
    std::string body;
    auto cb = [](const MD_CHAR *t, MD_SIZE s, void *u) { static_cast<std::string*>(u)->append(t,s); };
    if (md_html(md.c_str(), (MD_SIZE)md.size(), cb, &body,
        MD_FLAG_TABLES|MD_FLAG_STRIKETHROUGH|MD_FLAG_TASKLISTS|MD_FLAG_PERMISSIVEAUTOLINKS, 0) != 0)
        body = "<p style=\"color:#c33\">Error parsing markdown</p>";
    return "<html><head><style>"
        "body{font-family:Sans;font-size:16px;line-height:1.6;color:#222;padding:20px}"
        "h1{font-size:32px;margin-top:24px;margin-bottom:12px;border-bottom:1px solid #ccc;padding-bottom:8px}"
        "h2{font-size:26px;margin-top:20px;margin-bottom:10px;border-bottom:1px solid #eee;padding-bottom:6px}"
        "h3{font-size:22px;margin-top:16px;margin-bottom:8px}h4{font-size:18px;margin-top:14px;margin-bottom:6px}"
        "p{margin-top:8px;margin-bottom:8px}"
        "code{background-color:#f4f4f4;padding:2px 4px;font-family:Monospace;font-size:14px}"
        "pre{background-color:#f4f4f4;padding:12px;font-family:Monospace;font-size:14px;border:1px solid #ddd}"
        "pre code{padding:0;background-color:transparent}"
        "blockquote{margin-left:0;padding-left:16px;border-left:4px solid #ddd;color:#555}"
        "a{color:#0066cc}table{border-collapse:collapse;margin:12px 0}"
        "th,td{border:1px solid #ddd;padding:8px}th{background-color:#f0f0f0;font-weight:bold}"
        "ul,ol{padding-left:24px}li{margin-bottom:4px}"
        "hr{border:none;border-top:1px solid #ddd;margin:16px 0}"
        "del{text-decoration:line-through}img{max-width:100%}"
        "</style></head><body>" + body + "</body></html>";
}

/* ========================================================================= */

static constexpr int    SB_W = 8;
static constexpr double SB_MIN = 20.0;
static constexpr int    AUTOSCROLL_SPD = 8;
static constexpr double KEY_SPD = 4.0, PAGE_SPD = 30.0;
static constexpr double ZOOM_MIN = 0.25, ZOOM_MAX = 5.0, ZOOM_STEP = 0.1;
static constexpr int    SEARCH_BAR_H = 36;

/* ========================================================================= */

struct App {
    CairoContainer con;
    litehtml::document::ptr doc;
    std::string html;

    double scroll_x = 0, scroll_y = 0;
    int doc_w = 0, doc_h = 0;     /* document dims at zoom=1 */
    int vp_w = 900, vp_h = 700;
    double zoom = 1.0;
    double pinch_base_zoom = 1.0;

    bool sb_v_drag = false, sb_h_drag = false;
    double sb_drag_off = 0;
    bool ptr_outside = false;
    int autoscroll_dir = 0, leave_x = 0;
    double scroll_vel = 0;
    bool key_scrolling = false;

    /* Track space key state for shift-toggle */
    bool space_held = false;
    int  mouse_x = 0, mouse_y = 0;  /* last known content-area mouse pos */

    /* Search */
    bool search_active = false;
    std::string search_query;
    std::vector<SearchHit> search_matches;
    int current_match = -1;
    bool match_count_dirty = true;  /* re-run find_matches on next render */
};

static int max_sy(App *a) { return std::max(0, (int)(a->doc_h * a->zoom) - a->vp_h); }
static int max_sx(App *a) { return std::max(0, (int)(a->doc_w * a->zoom) - a->vp_w); }

/* Clamp for user scrolling — no whitespace when content overflows */
static void clamp(App *a) {
    double cw = a->doc_w * a->zoom, ch = a->doc_h * a->zoom;
    if (cw <= a->vp_w) a->scroll_x = std::clamp(a->scroll_x, -(a->vp_w - cw), 0.0);
    else               a->scroll_x = std::clamp(a->scroll_x, 0.0, cw - a->vp_w);
    if (ch <= a->vp_h) a->scroll_y = std::clamp(a->scroll_y, -(a->vp_h - ch), 0.0);
    else               a->scroll_y = std::clamp(a->scroll_y, 0.0, ch - a->vp_h);
}

static bool tick_on(App *a) { return a->key_scrolling || a->autoscroll_dir != 0; }

static void do_layout(App *a) {
    if (!a->doc) return;
    a->con.set_viewport(a->vp_w, a->vp_h);
    a->doc->render(a->vp_w, litehtml::render_all);
    a->doc_w = a->doc->width();
    a->doc_h = a->doc->height() + 40;
    a->match_count_dirty = true;  /* layout changed → positions invalid */
    clamp(a);
}

/* Compute new scroll for one axis during zoom.

   The scroll value is the screen-pixel offset of doc-origin from viewport-origin,
   negated. `offset = -scroll` is the screen pos of doc x=0 (or y=0).

   Three anchoring strategies, applied in this order:

   (1) Edge pinning on zoom-out: if a content edge sits on a viewport edge,
       keep it there. The user scrolled to that edge for a reason — peeling
       content away from it on zoom-out is jarring.

   (2) Focus-point zoom when the cursor is over content: the doc point under
       the cursor stays under the cursor. This is the classic "zoom to where
       I'm pointing" behavior.

   (3) Cursor-in-whitespace: there's no doc point under the cursor to anchor
       to, so we anchor the content's CENTER to its current screen position.
       The naive focus-point formula here would extrapolate a doc point
       outside the document, then clamp() would yank content to an edge —
       which is the "wonky jump" symptom. */
static double zoom_axis(double scroll, double old_zoom, double new_zoom,
                        int doc_size, int vp, int focus) {
    double old_cs = doc_size * old_zoom;
    double new_cs = doc_size * new_zoom;
    double old_offset = -scroll;
    const double EPS = 0.5;
    bool zooming_in = new_zoom > old_zoom;

    /* (1) Edge pinning — only on zoom-out, only if exactly one edge is pinned. */
    if (!zooming_in) {
        bool left_at_edge  = std::abs(old_offset) < EPS;
        bool right_at_edge = std::abs(old_offset + old_cs - vp) < EPS;
        if (left_at_edge && !right_at_edge)  return 0.0;
        if (right_at_edge && !left_at_edge)  return new_cs - vp;
    }

    /* (2) Cursor over content: focus-point zoom. */
    bool over_content = (focus >= old_offset - EPS && focus <= old_offset + old_cs + EPS);
    if (over_content) {
        double doc_pt = (focus - old_offset) / old_zoom;
        return doc_pt * new_zoom - focus;
    }

    /* (3) Cursor in whitespace: anchor content's center at its current screen position. */
    double anchor_doc = doc_size / 2.0;
    double anchor_screen = old_offset + old_cs / 2.0;
    return anchor_doc * new_zoom - anchor_screen;
}

static void apply_zoom(App *a, double nz, int fx, int fy) {
    nz = std::clamp(nz, ZOOM_MIN, ZOOM_MAX);
    if (std::abs(nz - a->zoom) < 0.001) return;
    double oz = a->zoom;
    a->scroll_x = zoom_axis(a->scroll_x, oz, nz, a->doc_w, a->vp_w, fx);
    a->scroll_y = zoom_axis(a->scroll_y, oz, nz, a->doc_h, a->vp_h, fy);
    a->zoom = nz;
    a->match_count_dirty = true;  /* font sizes unchanged but x positions scaled inside spans */
    clamp(a);
}

/* Scrollbar geometry */
struct SBG { double pos, len, track; bool vis; };

static SBG sbv(App *a) {
    SBG g{}; int sdh = (int)(a->doc_h * a->zoom);
    g.vis = sdh > a->vp_h; if (!g.vis) return g;
    g.track = a->vp_h;
    double r = (double)a->vp_h / sdh;
    g.len = std::max(SB_MIN, r * a->vp_h);
    double ms = sdh - a->vp_h;
    g.pos = (ms > 0) ? (a->scroll_y / ms) * (a->vp_h - g.len) : 0;
    return g;
}

static SBG sbh(App *a) {
    SBG g{}; int sdw = (int)(a->doc_w * a->zoom);
    g.vis = sdw > a->vp_w; if (!g.vis) return g;
    g.track = a->vp_w;
    double r = (double)a->vp_w / sdw;
    g.len = std::max(SB_MIN, r * a->vp_w);
    double ms = sdw - a->vp_w;
    g.pos = (ms > 0) ? (a->scroll_x / ms) * (a->vp_w - g.len) : 0;
    return g;
}

/* ========================================================================= */
/* Search                                                                    */
/* ========================================================================= */

/* Scroll only if the current match is outside the visible viewport (minus
   the search bar at top). Avoids jarring re-centering when the match is
   already on screen. */
static bool ensure_match_visible(App *a) {
    if (a->current_match < 0 || a->current_match >= (int)a->search_matches.size()) return false;
    const SearchHit &h = a->search_matches[a->current_match];
    double top = (double)h.doc_y * a->zoom;
    double bot = (double)(h.doc_y + h.height) * a->zoom;
    double margin_top = SEARCH_BAR_H + 10;
    double margin_bot = 10;
    if (top < a->scroll_y + margin_top || bot > a->scroll_y + a->vp_h - margin_bot) {
        a->scroll_y = top - a->vp_h * 0.4;
        clamp(a);
        return true;
    }
    return false;
}

static void close_search(App *a, mdv_window *w) {
    a->search_active = false;
    a->search_query.clear();
    a->search_matches.clear();
    a->current_match = -1;
    a->match_count_dirty = false;
    mdv_window_request_redraw(w);
}

static void open_search(App *a, mdv_window *w) {
    a->search_active = true;
    a->search_query.clear();
    a->search_matches.clear();
    a->current_match = -1;
    a->match_count_dirty = false;
    mdv_window_request_redraw(w);
}

static void draw_search_bar(App *a, cairo_t *cr, int ww) {
    int bh = SEARCH_BAR_H;

    cairo_set_source_rgba(cr, 0.97, 0.97, 0.97, 0.97);
    cairo_rectangle(cr, 0, 0, ww, bh); cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.55, 0.55, 0.55, 1.0);
    cairo_rectangle(cr, 0, bh - 1, ww, 1); cairo_fill(cr);

    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 13");
    pango_layout_set_font_description(l, fd);

    std::string text = "Find: " + a->search_query;
    pango_layout_set_text(l, text.c_str(), -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(l, &tw, &th);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
    double ty = (bh - th) / 2.0;
    cairo_move_to(cr, 12, ty);
    pango_cairo_show_layout(cr, l);

    /* Caret */
    cairo_rectangle(cr, 12 + tw + 1, ty, 2, th);
    cairo_fill(cr);

    /* Match count or status */
    std::string count;
    if (a->search_query.empty())            count = "Ctrl+F · Enter: next · Shift+Enter: prev · Esc: close";
    else if (a->search_matches.empty())     count = "No matches";
    else                                    count = std::to_string(a->current_match + 1) + " / " +
                                                    std::to_string(a->search_matches.size());

    pango_layout_set_text(l, count.c_str(), -1);
    int cw = 0, ch = 0;
    pango_layout_get_pixel_size(l, &cw, &ch);
    cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);
    cairo_move_to(cr, ww - cw - 12, (bh - ch) / 2.0);
    pango_cairo_show_layout(cr, l);

    pango_font_description_free(fd);
    g_object_unref(l);
}

/* ========================================================================= */

static void on_redraw(mdv_window *w, cairo_t *cr, int ww, int hh, void *ud) {
    App *a = (App*)ud;
    cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr);
    if (!a->doc) return;

    int dsx = (int)(a->scroll_x / a->zoom), dsy = (int)(a->scroll_y / a->zoom);
    a->con.set_scroll_offset(dsy);
    a->con.set_scroll_offset_x(dsx);
    a->con.begin_record();

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, ww, hh); cairo_clip(cr);
    cairo_scale(cr, a->zoom, a->zoom);

    int rw = (int)(ww / a->zoom), rh = (int)(hh / a->zoom);
    litehtml::position clip(0, 0, rw, rh);
    a->con.set_render_cr(cr);
    a->doc->draw((litehtml::uint_ptr)cr, -dsx, -dsy, &clip);
    a->con.set_render_cr(nullptr);

    /* Re-find matches against freshly-recorded spans if layout changed since last search */
    if (a->search_active && !a->search_query.empty() && a->match_count_dirty) {
        int prev_cm = a->current_match;
        a->search_matches = a->con.find_matches(a->search_query);
        if (a->search_matches.empty()) {
            a->current_match = -1;
        } else if (prev_cm < 0 || prev_cm >= (int)a->search_matches.size()) {
            a->current_match = 0;
            if (ensure_match_visible(a)) mdv_window_request_redraw(w);
        }
        a->match_count_dirty = false;
    }

    a->con.draw_search_matches(cr, a->search_matches, a->current_match);
    a->con.draw_selection_overlay(cr);
    cairo_restore(cr);

    /* Vertical scrollbar */
    SBG sv = sbv(a);
    if (sv.vis) {
        double x = ww - SB_W;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.05);
        cairo_rectangle(cr, x, 0, SB_W - 2, sv.track); cairo_fill(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, a->sb_v_drag ? 0.45 : 0.25);
        cairo_rectangle(cr, x, sv.pos, SB_W - 2, sv.len); cairo_fill(cr);
    }

    /* Horizontal scrollbar */
    SBG sh = sbh(a);
    if (sh.vis) {
        double y = hh - SB_W;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.05);
        cairo_rectangle(cr, 0, y, sh.track, SB_W - 2); cairo_fill(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, a->sb_h_drag ? 0.45 : 0.25);
        cairo_rectangle(cr, sh.pos, y, sh.len, SB_W - 2); cairo_fill(cr);
    }

    /* Search bar overlay (drawn last, in viewport coords) */
    if (a->search_active) draw_search_bar(a, cr, ww);
}

static void on_resize(mdv_window *w, int ww, int hh, void *ud) {
    App *a = (App*)ud; a->vp_w = ww; a->vp_h = hh;
    do_layout(a); mdv_window_request_redraw(w);
}

static void on_scroll(mdv_window *w, int x, int y, double dx, double dy, uint32_t mods, void *ud) {
    App *a = (App*)ud;
    if (mods & MDV_MOD_CTRL) {
        apply_zoom(a, a->zoom + ((dy < 0) ? ZOOM_STEP : -ZOOM_STEP), x, y);
    } else if (mods & MDV_MOD_SHIFT) {
        a->scroll_x += (dy + dx) * 3.0;
        clamp(a);
    } else {
        a->scroll_y += dy * 3.0;
        a->scroll_x += dx * 3.0;
        clamp(a);
    }
    mdv_window_request_redraw(w);
}

static void on_mouse_move(mdv_window *w, int x, int y, void *ud) {
    App *a = (App*)ud;
    a->mouse_x = x; a->mouse_y = y;

    if (a->sb_v_drag) {
        SBG sv = sbv(a); double mt = a->vp_h - sv.len;
        if (mt > 0) { a->scroll_y = std::clamp((y - a->sb_drag_off) / mt, 0.0, 1.0) * max_sy(a); clamp(a); }
        mdv_window_request_redraw(w); return;
    }
    if (a->sb_h_drag) {
        SBG sh = sbh(a); double mt = a->vp_w - sh.len;
        if (mt > 0) { a->scroll_x = std::clamp((x - a->sb_drag_off) / mt, 0.0, 1.0) * max_sx(a); clamp(a); }
        mdv_window_request_redraw(w); return;
    }

    /* Cursor: arrow over scrollbars and search bar, text elsewhere */
    { SBG sv = sbv(a); SBG sh = sbh(a);
      bool over_sb = (sv.vis && x >= a->vp_w - SB_W) || (sh.vis && y >= a->vp_h - SB_W);
      bool over_search_bar = a->search_active && y < SEARCH_BAR_H;
      mdv_window_set_cursor(w, (over_sb || over_search_bar) ? "default" : "text"); }

    Selection &sel = a->con.selection();
    if (sel.dragging) {
        sel.extent_x = (int)((x + a->scroll_x) / a->zoom);
        sel.extent_y = (int)((y + a->scroll_y) / a->zoom);
        sel.active = true;
        mdv_window_request_redraw(w); return;
    }

    if (!a->doc) return;
    int dx2 = (int)((x + a->scroll_x) / a->zoom), dy2 = (int)((y + a->scroll_y) / a->zoom);
    std::vector<litehtml::position> rb;
    if (a->doc->on_mouse_over(dx2, dy2, dx2, dy2, rb)) mdv_window_request_redraw(w);
}

static void on_mouse_button(mdv_window *w, int x, int y, int btn, uint32_t mods, bool pr, void *ud) {
    App *a = (App*)ud;
    if (btn == BTN_LEFT) {
        if (pr) {
            /* Vertical scrollbar */
            SBG sv = sbv(a);
            if (sv.vis && x >= a->vp_w - SB_W) {
                if (y >= sv.pos && y <= sv.pos + sv.len) { a->sb_v_drag = true; a->sb_drag_off = y - sv.pos; }
                else { double mt = a->vp_h - sv.len; if (mt > 0) { a->scroll_y = std::clamp((y - sv.len/2.0) / mt, 0.0, 1.0) * max_sy(a); clamp(a); } }
                mdv_window_request_redraw(w); return;
            }
            /* Horizontal scrollbar */
            SBG sh = sbh(a);
            if (sh.vis && y >= a->vp_h - SB_W) {
                if (x >= sh.pos && x <= sh.pos + sh.len) { a->sb_h_drag = true; a->sb_drag_off = x - sh.pos; }
                else { double mt = a->vp_w - sh.len; if (mt > 0) { a->scroll_x = std::clamp((x - sh.len/2.0) / mt, 0.0, 1.0) * max_sx(a); clamp(a); } }
                mdv_window_request_redraw(w); return;
            }
            /* Click in search bar: keep focus, don't start selection */
            if (a->search_active && y < SEARCH_BAR_H) return;

            Selection &sel = a->con.selection();
            int dx = (int)((x + a->scroll_x) / a->zoom), dy = (int)((y + a->scroll_y) / a->zoom);
            if ((mods & MDV_MOD_SHIFT) && sel.active) { sel.extent_x = dx; sel.extent_y = dy; sel.dragging = true; }
            else { sel.anchor_x = dx; sel.anchor_y = dy; sel.extent_x = dx; sel.extent_y = dy; sel.dragging = true; sel.active = false; }
            mdv_window_request_redraw(w);
        } else {
            if (a->sb_v_drag) { a->sb_v_drag = false; mdv_window_request_redraw(w); return; }
            if (a->sb_h_drag) { a->sb_h_drag = false; mdv_window_request_redraw(w); return; }
            Selection &sel = a->con.selection();
            if (sel.dragging) {
                sel.dragging = false; a->autoscroll_dir = 0; a->ptr_outside = false;
                mdv_window_set_tick_enabled(w, tick_on(a));
                int ddx = sel.anchor_x - sel.extent_x, ddy = sel.anchor_y - sel.extent_y;
                if (ddx*ddx + ddy*ddy < 16) sel.clear();
                mdv_window_request_redraw(w);
            }
        }
    }
    if (!a->doc) return;
    int dx = (int)((x + a->scroll_x) / a->zoom), dy = (int)((y + a->scroll_y) / a->zoom);
    std::vector<litehtml::position> rb;
    if (pr) a->doc->on_lbutton_down(dx, dy, dx, dy, rb);
    else    a->doc->on_lbutton_up(dx, dy, dx, dy, rb);
}

static void on_pointer_leave(mdv_window *w, int lx, int ly, void *ud) {
    App *a = (App*)ud; Selection &sel = a->con.selection();
    if (!sel.dragging) return;
    a->ptr_outside = true; a->leave_x = (int)((lx + a->scroll_x) / a->zoom);
    a->autoscroll_dir = (ly < a->vp_h / 2) ? -1 : 1;
    mdv_window_set_tick_enabled(w, true);
}

static void on_pointer_enter(mdv_window *w, int x, int y, void *ud) {
    App *a = (App*)ud; a->ptr_outside = false; a->autoscroll_dir = 0;
    mdv_window_set_tick_enabled(w, tick_on(a));
    Selection &sel = a->con.selection();
    if (sel.dragging) {
        sel.extent_x = (int)((x + a->scroll_x) / a->zoom);
        sel.extent_y = (int)((y + a->scroll_y) / a->zoom);
        sel.active = true; mdv_window_request_redraw(w);
    }
}

static void on_pinch(mdv_window *w, double scale, int cx, int cy, bool finished, void *ud) {
    App *a = (App*)ud;
    if (finished) return;
    if (std::abs(scale - 1.0) < 0.001) { a->pinch_base_zoom = a->zoom; return; }
    apply_zoom(a, a->pinch_base_zoom * scale, cx, cy);
    mdv_window_request_redraw(w);
}

static void on_tick(mdv_window *w, void *ud) {
    App *a = (App*)ud; bool dirty = false;
    Selection &sel = a->con.selection();
    if (sel.dragging && a->autoscroll_dir != 0) {
        a->scroll_y += a->autoscroll_dir * AUTOSCROLL_SPD; clamp(a);
        sel.extent_x = a->leave_x;
        sel.extent_y = (a->autoscroll_dir < 0) ? (int)(a->scroll_y / a->zoom) : (int)((a->scroll_y + a->vp_h) / a->zoom);
        sel.active = true; dirty = true;
    }
    if (a->key_scrolling && a->scroll_vel != 0) { a->scroll_y += a->scroll_vel; clamp(a); dirty = true; }
    if (!tick_on(a)) mdv_window_set_tick_enabled(w, false);
    if (dirty) mdv_window_request_redraw(w);
}

static void start_ks(App *a, mdv_window *w, double v) { a->scroll_vel = v; a->key_scrolling = true; mdv_window_set_tick_enabled(w, true); }
static void stop_ks(App *a, mdv_window *w) { a->scroll_vel = 0; a->key_scrolling = false; a->space_held = false; if (!tick_on(a)) mdv_window_set_tick_enabled(w, false); }

/* Remove last UTF-8 codepoint from `s` */
static void utf8_pop_back(std::string &s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (((unsigned char)s[i]) & 0xC0) == 0x80) i--;
    s.erase(i);
}

static void navigate_match(App *a, mdv_window *w, bool backward) {
    if (a->search_matches.empty()) return;
    int n = (int)a->search_matches.size();
    if (a->current_match < 0) a->current_match = backward ? n - 1 : 0;
    else if (backward)        a->current_match = (a->current_match - 1 + n) % n;
    else                      a->current_match = (a->current_match + 1) % n;
    ensure_match_visible(a);
    mdv_window_request_redraw(w);
}

static void on_key(mdv_window *w, uint32_t ks, uint32_t mods, bool pr, void *ud) {
    App *a = (App*)ud;

    /* ---- Search-mode key handling (only on key press) ---- */
    if (a->search_active && pr) {
        if (ks == XKB_KEY_Escape) { close_search(a, w); return; }
        if (ks == XKB_KEY_Return || ks == XKB_KEY_KP_Enter) {
            navigate_match(a, w, mods & MDV_MOD_SHIFT);
            return;
        }
        if (ks == XKB_KEY_BackSpace) {
            if (!a->search_query.empty()) {
                utf8_pop_back(a->search_query);
                a->match_count_dirty = true;
                if (a->search_query.empty()) {
                    a->search_matches.clear();
                    a->current_match = -1;
                }
                mdv_window_request_redraw(w);
            }
            return;
        }
        if (mods & MDV_MOD_CTRL) {
            if (ks == XKB_KEY_f) { close_search(a, w); return; }
            /* Ctrl+C / Ctrl+A still operate on the document — fall through */
        } else {
            /* Try printable text */
            char buf[8];
            int n = xkb_keysym_to_utf8(ks, buf, sizeof(buf));
            if (n > 1) {
                unsigned char c0 = (unsigned char)buf[0];
                if (c0 >= 32 && c0 != 127) {
                    a->search_query.append(buf, n - 1);
                    a->match_count_dirty = true;
                    mdv_window_request_redraw(w);
                }
            }
            /* Eat anything else (arrows, page nav, etc) while typing */
            return;
        }
    }

    /* ---- Normal key handling ---- */

    if (!pr) {
        switch (ks) {
        case XKB_KEY_j: case XKB_KEY_Down: case XKB_KEY_k: case XKB_KEY_Up:
        case XKB_KEY_Page_Down: case XKB_KEY_Page_Up:
            stop_ks(a, w); break;
        case XKB_KEY_space:
            stop_ks(a, w); a->space_held = false; break;
        case XKB_KEY_Shift_L: case XKB_KEY_Shift_R:
            if (a->space_held && a->key_scrolling)
                a->scroll_vel = PAGE_SPD;
            break;
        }
        return;
    }

    if ((ks == XKB_KEY_Shift_L || ks == XKB_KEY_Shift_R) && a->space_held && a->key_scrolling) {
        a->scroll_vel = -PAGE_SPD;
        return;
    }

    if ((mods & MDV_MOD_CTRL) && ks == XKB_KEY_f) {
        if (a->search_active) close_search(a, w);
        else                  open_search(a, w);
        return;
    }
    if ((mods & MDV_MOD_CTRL) && ks == XKB_KEY_c) {
        std::string t = a->con.get_selected_text();
        if (!t.empty()) mdv_window_copy_to_clipboard(w, t.c_str()); return;
    }
    if ((mods & MDV_MOD_CTRL) && ks == XKB_KEY_a) {
        Selection &s = a->con.selection();
        s.anchor_x = 0; s.anchor_y = 0;
        s.extent_x = a->doc_w; s.extent_y = a->doc_h;
        s.active = true; s.dragging = false; mdv_window_request_redraw(w); return;
    }
    if (mods & MDV_MOD_CTRL) {
        if (ks == XKB_KEY_equal || ks == XKB_KEY_plus) { apply_zoom(a, a->zoom + ZOOM_STEP, a->mouse_x, a->mouse_y); mdv_window_request_redraw(w); return; }
        if (ks == XKB_KEY_minus) { apply_zoom(a, a->zoom - ZOOM_STEP, a->mouse_x, a->mouse_y); mdv_window_request_redraw(w); return; }
        if (ks == XKB_KEY_0) { apply_zoom(a, 1.0, a->mouse_x, a->mouse_y); mdv_window_request_redraw(w); return; }
    }
    /* F3 / Shift+F3 jump to next/prev match if search has matches */
    if (ks == XKB_KEY_F3 && !a->search_matches.empty()) {
        navigate_match(a, w, mods & MDV_MOD_SHIFT);
        return;
    }
    switch (ks) {
    case XKB_KEY_j: case XKB_KEY_Down:  start_ks(a, w, KEY_SPD); return;
    case XKB_KEY_k: case XKB_KEY_Up:    start_ks(a, w, -KEY_SPD); return;
    case XKB_KEY_Page_Down:             start_ks(a, w, PAGE_SPD); return;
    case XKB_KEY_Page_Up:               start_ks(a, w, -PAGE_SPD); return;
    case XKB_KEY_space:
        a->space_held = true;
        start_ks(a, w, (mods & MDV_MOD_SHIFT) ? -PAGE_SPD : PAGE_SPD); return;
    case XKB_KEY_Home: case XKB_KEY_g:  a->scroll_y = 0; mdv_window_request_redraw(w); return;
    case XKB_KEY_End:  case XKB_KEY_G:  a->scroll_y = max_sy(a); mdv_window_request_redraw(w); return;
    case XKB_KEY_q: case XKB_KEY_Escape: mdv_window_quit(w); return;
    }
}

static void on_close(mdv_window *w, void *ud) { (void)ud; mdv_window_quit(w); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: mdview <path-to-markdown-file>\n"); return 1; }
    std::ifstream f(argv[1]);
    if (!f.is_open()) { fprintf(stderr, "Error: cannot open '%s'\n", argv[1]); return 1; }
    std::ostringstream ss; ss << f.rdbuf(); f.close();

    App app;
    app.html = md2html(ss.str());
    { std::string fp = argv[1]; auto sl = fp.rfind('/');
      app.con.set_base_dir(sl != std::string::npos ? fp.substr(0, sl) : "."); }
    app.doc = litehtml::document::createFromString(app.html.c_str(), &app.con, litehtml::master_css);
    app.pinch_base_zoom = 1.0;

    mdv_callbacks cb = {};
    cb.on_redraw = on_redraw; cb.on_resize = on_resize; cb.on_scroll = on_scroll;
    cb.on_mouse_move = on_mouse_move; cb.on_mouse_button = on_mouse_button;
    cb.on_key = on_key; cb.on_close = on_close;
    cb.on_pointer_leave = on_pointer_leave; cb.on_pointer_enter = on_pointer_enter;
    cb.on_tick = on_tick; cb.on_pinch = on_pinch;
    cb.userdata = &app;

    std::string title = std::string("mdview - ") + argv[1];
    mdv_window *w = mdv_window_create(title.c_str(), 900, 700, &cb);
    if (!w) { fprintf(stderr, "Error: cannot create Wayland window\n"); return 1; }
    do_layout(&app); mdv_window_run(w); mdv_window_destroy(w);
    return 0;
}

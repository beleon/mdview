#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
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
#include "highlight.h"
#include "state.h"
#include "livereload.h"
extern "C" {
#include "md4c.h"
#include "md4c-html.h"
#include "window.h"
}

struct Theme {
    const char *body_bg;        const char *body_color;
    const char *h_border_strong; const char *h_border_light;
    const char *code_bg;        const char *code_border;
    const char *bq_border;      const char *bq_color;
    const char *link;
    const char *th_bg;          const char *border;
    const char *hr_border;
};
static const Theme THEME_LIGHT = {
    "#ffffff", "#222222",
    "#cccccc", "#eeeeee",
    "#f4f4f4", "#dddddd",
    "#dddddd", "#555555",
    "#0066cc",
    "#f0f0f0", "#dddddd",
    "#dddddd"
};
static const Theme THEME_DARK = {
    "#1e1e1e", "#d4d4d4",
    "#444444", "#333333",
    "#2a2a2a", "#3a3a3a",
    "#3a3a3a", "#999999",
    "#6db5ff",
    "#2a2a2a", "#3a3a3a",
    "#3a3a3a"
};

static std::string md2html(const std::string &md, bool dark) {
    std::string body;
    auto cb = [](const MD_CHAR *t, MD_SIZE s, void *u) { static_cast<std::string*>(u)->append(t,s); };
    if (md_html(md.c_str(), (MD_SIZE)md.size(), cb, &body,
        MD_FLAG_TABLES|MD_FLAG_STRIKETHROUGH|MD_FLAG_TASKLISTS|MD_FLAG_PERMISSIVEAUTOLINKS, 0) != 0)
        body = "<p style=\"color:#c33\">Error parsing markdown</p>";

    const Theme &t = dark ? THEME_DARK : THEME_LIGHT;
    char css[2048];
    snprintf(css, sizeof(css),
        "body{font-family:Sans;font-size:16px;line-height:1.6;color:%s;background:%s;padding:20px}"
        "h1{font-size:32px;margin-top:24px;margin-bottom:12px;border-bottom:1px solid %s;padding-bottom:8px}"
        "h2{font-size:26px;margin-top:20px;margin-bottom:10px;border-bottom:1px solid %s;padding-bottom:6px}"
        "h3{font-size:22px;margin-top:16px;margin-bottom:8px}h4{font-size:18px;margin-top:14px;margin-bottom:6px}"
        "p{margin-top:8px;margin-bottom:8px}"
        "code{background-color:%s;padding:2px 4px;font-family:Monospace;font-size:14px}"
        "pre{background-color:%s;padding:12px;font-family:Monospace;font-size:14px;border:1px solid %s}"
        "pre code{padding:0;background-color:transparent}"
        "blockquote{margin-left:0;padding-left:16px;border-left:4px solid %s;color:%s}"
        "a{color:%s}table{border-collapse:collapse;margin:12px 0}"
        "th,td{border:1px solid %s;padding:8px}th{background-color:%s;font-weight:bold}"
        "ul,ol{padding-left:24px}li{margin-bottom:4px}"
        "hr{border:none;border-top:1px solid %s;margin:16px 0}"
        "del{text-decoration:line-through}img{max-width:100%%}",
        t.body_color, t.body_bg,
        t.h_border_strong, t.h_border_light,
        t.code_bg, t.code_bg, t.code_border,
        t.bq_border, t.bq_color,
        t.link, t.border, t.th_bg, t.hr_border);
    return std::string("<html><head><style>") + css + "</style></head><body>" + body + "</body></html>";
}

/* GitHub-ish slug: lowercase, alphanumerics kept, runs of other chars
   collapse to a single '-'. Used to give headings stable #ids for
   in-document anchor links. */
static std::string slugify(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    bool prev_dash = false;
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (isalnum(u)) {
            out += (char)tolower(u);
            prev_dash = false;
        } else if (u >= 0x80) {
            out += c;          /* keep multibyte UTF-8 verbatim */
            prev_dash = false;
        } else {
            if (!prev_dash && !out.empty()) out += '-';
            prev_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

/* Walk the HTML string and inject id="slug-of-text" into each <hN> opening
   tag that doesn't already have one. Duplicate slugs get -2, -3, ... */
static std::string add_heading_ids(const std::string &html) {
    std::string out;
    out.reserve(html.size() + 128);
    std::map<std::string, int> seen;
    size_t pos = 0;
    while (pos < html.size()) {
        if (pos + 3 < html.size() &&
            html[pos] == '<' && html[pos+1] == 'h' &&
            html[pos+2] >= '1' && html[pos+2] <= '6' &&
            (html[pos+3] == '>' || html[pos+3] == ' ')) {
            char level = html[pos+2];
            char close = '>';
            size_t open_end = html.find(close, pos);
            if (open_end == std::string::npos) { out += html[pos++]; continue; }
            std::string opening = html.substr(pos, open_end - pos + 1);
            bool has_id = opening.find(" id=") != std::string::npos
                       || opening.find("\tid=") != std::string::npos;
            std::string end_tag = "</h"; end_tag += level; end_tag += ">";
            size_t close_pos = html.find(end_tag, open_end + 1);
            if (close_pos == std::string::npos) { out += html[pos++]; continue; }
            std::string inner = html.substr(open_end + 1, close_pos - open_end - 1);

            if (!has_id) {
                /* Strip nested tags + decode entities for slug input. */
                std::string text;
                bool in_tag = false;
                for (char c : inner) {
                    if (c == '<') in_tag = true;
                    else if (c == '>') in_tag = false;
                    else if (!in_tag) text += c;
                }
                text = mdv::html_decode(text);
                std::string slug = slugify(text);
                if (!slug.empty()) {
                    int n = ++seen[slug];
                    std::string final_slug = (n == 1) ? slug : (slug + "-" + std::to_string(n));
                    /* Insert id attribute just before the closing '>'. */
                    out.append(opening, 0, opening.size() - 1);
                    out += " id=\"";
                    out += final_slug;
                    out += "\">";
                } else {
                    out += opening;
                }
            } else {
                out += opening;
            }
            out += inner;
            out += end_tag;
            pos = close_pos + end_tag.size();
        } else {
            out += html[pos++];
        }
    }
    return out;
}

/* ========================================================================= */

static constexpr int    SB_W = 8;
static constexpr double SB_MIN = 20.0;
static constexpr int    AUTOSCROLL_SPD = 8;
static constexpr double KEY_SPD = 4.0, PAGE_SPD = 30.0;
static constexpr double ZOOM_MIN = 0.25, ZOOM_MAX = 5.0, ZOOM_STEP = 0.1;
static constexpr int    SEARCH_BAR_H = 36;
static constexpr int    STATUS_H = 22;
static constexpr int    SIDEBAR_W = 280;
static constexpr int    TOC_LINE_H = 22;
static constexpr int    TOC_PAD_TOP = 10;

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
    bool case_sensitive = false;
    bool whole_word = false;
    bool regex_mode = false;
    std::string search_query;
    std::vector<SearchHit> search_matches;
    int current_match = -1;
    bool match_count_dirty = true;  /* re-run find_matches on next render */

    /* Syntax highlighting (async via subprocess) */
    std::string original_html;   /* pre-highlight, kept for rebuild substitution */
    mdv::Highlighter highlighter;

    /* Window handle (set after creation) */
    mdv_window *win = nullptr;

    /* Help overlay */
    bool help_visible = false;

    /* File + live reload */
    std::string file_path;
    std::string file_basename;
    std::string source_md;     /* raw markdown source (for dark-mode + reload re-render) */
    mdv::LiveReloader live_reloader;

    /* Theme */
    bool dark_mode = false;

    /* TOC sidebar */
    struct TocEntry { int level; std::string text; std::string id; };
    bool                  toc_visible = false;
    std::vector<TocEntry> toc_entries;
    double                toc_scroll_y = 0;
    int                   toc_hover_idx = -1;

    /* Right-click context menu (drawn as an in-window overlay) */
    enum class MenuAction { OpenLink, CopyLink, Copy, SelectAll, Find, Reload, ToggleDark };
    struct MenuItem { std::string label; MenuAction action; std::string param; bool enabled; };
    struct ContextMenu {
        bool visible = false;
        int x = 0, y = 0, w = 0, h = 0;
        int hover_idx = -1;
        std::vector<MenuItem> items;
    } menu;
};

static int max_sy(App *a) { return std::max(0, (int)(a->doc_h * a->zoom) - a->vp_h); }
static int max_sx(App *a) { return std::max(0, (int)(a->doc_w * a->zoom) - a->vp_w); }
static int toc_w(App *a)  { return a->toc_visible ? SIDEBAR_W : 0; }

/* Parse <h1..h3 id="..">text</hN> out of the HTML and return a flat list.
   Run on the post-add_heading_ids string so every heading has an id. */
static std::vector<App::TocEntry> extract_toc(const std::string &html) {
    std::vector<App::TocEntry> r;
    size_t pos = 0;
    while (pos < html.size()) {
        if (pos + 3 < html.size() && html[pos] == '<' && html[pos+1] == 'h'
            && html[pos+2] >= '1' && html[pos+2] <= '3'
            && (html[pos+3] == '>' || html[pos+3] == ' ')) {
            int level = html[pos+2] - '0';
            size_t open_end = html.find('>', pos);
            if (open_end == std::string::npos) break;
            std::string opening = html.substr(pos, open_end - pos + 1);
            std::string id;
            size_t id_pos = opening.find(" id=\"");
            if (id_pos != std::string::npos) {
                size_t id_end = opening.find('"', id_pos + 5);
                if (id_end != std::string::npos) id = opening.substr(id_pos + 5, id_end - id_pos - 5);
            }
            std::string end_tag = "</h"; end_tag += html[pos+2]; end_tag += ">";
            size_t close = html.find(end_tag, open_end);
            if (close == std::string::npos) break;
            std::string raw = html.substr(open_end + 1, close - open_end - 1);
            std::string text;
            bool in_tag = false;
            for (char c : raw) {
                if (c == '<')      in_tag = true;
                else if (c == '>') in_tag = false;
                else if (!in_tag)  text += c;
            }
            text = mdv::html_decode(text);
            r.push_back({level, std::move(text), std::move(id)});
            pos = close + end_tag.size();
        } else {
            pos++;
        }
    }
    return r;
}

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

/* Focus-point zoom — Google-Maps style. The doc point at viewport position
   `focus` stays at viewport position `focus` after the zoom. Bounds
   enforcement is delegated entirely to clamp(): when content fits the
   viewport, clamp() may pull the result to a valid range, which accepts a
   small focus-point drift as the price of not showing whitespace where
   content used to be. */
static double zoom_axis(double scroll, double old_zoom, double new_zoom,
                        int /*doc_size*/, int /*vp*/, int focus) {
    double doc_pt = (scroll + focus) / old_zoom;
    return doc_pt * new_zoom - focus;
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

/* Chrome colors: same shape, different palette per theme. */
struct Chrome {
    double bg[3];
    double text[3];
    double dim[3];
    double sep[3];
    double accent[3];
    double accent_bg[4];  /* with alpha */
};
static const Chrome CHROME_LIGHT = {
    {0.97,0.97,0.97},
    {0.10,0.10,0.10},
    {0.45,0.45,0.45},
    {0.60,0.60,0.60},
    {0.20,0.50,0.90},
    {0.20,0.50,0.90, 0.18}
};
static const Chrome CHROME_DARK = {
    {0.16,0.16,0.16},
    {0.85,0.85,0.85},
    {0.60,0.60,0.60},
    {0.30,0.30,0.30},
    {0.50,0.70,1.00},
    {0.40,0.60,1.00, 0.20}
};
static const Chrome &chrome(App *a) { return a->dark_mode ? CHROME_DARK : CHROME_LIGHT; }

static int toc_total_height(App *a) {
    return TOC_PAD_TOP * 2 + (int)a->toc_entries.size() * TOC_LINE_H;
}

static void clamp_toc_scroll(App *a) {
    int total = toc_total_height(a);
    double max = std::max(0, total - a->vp_h);
    if (a->toc_scroll_y < 0)   a->toc_scroll_y = 0;
    if (a->toc_scroll_y > max) a->toc_scroll_y = max;
}

static void draw_sidebar(App *a, cairo_t *cr) {
    const Chrome &c = chrome(a);
    int w = SIDEBAR_W;
    int h = a->vp_h;

    cairo_set_source_rgb(cr, c.bg[0], c.bg[1], c.bg[2]);
    cairo_rectangle(cr, 0, 0, w, h); cairo_fill(cr);
    cairo_set_source_rgb(cr, c.sep[0], c.sep[1], c.sep[2]);
    cairo_rectangle(cr, w - 1, 0, 1, h); cairo_fill(cr);

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, w, h); cairo_clip(cr);

    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 11");
    pango_layout_set_font_description(l, fd);

    /* Header label */
    pango_layout_set_text(l, "Outline", -1);
    int th = 0, tw_px = 0;
    pango_layout_get_pixel_size(l, &tw_px, &th);
    cairo_set_source_rgb(cr, c.dim[0], c.dim[1], c.dim[2]);
    cairo_move_to(cr, 14, 6);
    pango_cairo_show_layout(cr, l);
    cairo_set_source_rgb(cr, c.sep[0], c.sep[1], c.sep[2]);
    cairo_rectangle(cr, 12, 6 + th + 4, w - 24, 1); cairo_fill(cr);

    /* Entries */
    int y0 = TOC_PAD_TOP - (int)a->toc_scroll_y + 24;  /* offset for header */
    for (size_t i = 0; i < a->toc_entries.size(); i++) {
        const auto &e = a->toc_entries[i];
        int y = y0 + (int)i * TOC_LINE_H;
        if (y + TOC_LINE_H < 24) continue;
        if (y > h) break;

        if ((int)i == a->toc_hover_idx) {
            cairo_set_source_rgba(cr, c.accent_bg[0], c.accent_bg[1], c.accent_bg[2], c.accent_bg[3]);
            cairo_rectangle(cr, 4, y, w - 8, TOC_LINE_H); cairo_fill(cr);
        }
        int indent = 14 + (e.level - 1) * 14;
        pango_layout_set_text(l, e.text.c_str(), -1);
        pango_layout_set_width(l, (w - indent - 12) * PANGO_SCALE);
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
        int eh = 0, ew = 0;
        pango_layout_get_pixel_size(l, &ew, &eh);
        cairo_set_source_rgb(cr, c.text[0], c.text[1], c.text[2]);
        cairo_move_to(cr, indent, y + (TOC_LINE_H - eh) / 2.0);
        pango_cairo_show_layout(cr, l);
        pango_layout_set_width(l, -1);
    }

    pango_font_description_free(fd);
    g_object_unref(l);
    cairo_restore(cr);
}

/* Context menu helpers (forward declarations of functions that reference
   App's nested types). Implementations follow draw_search_bar. */
static void   menu_close(App *a);
static int    menu_item_at(App *a, int x, int y);
static void   draw_context_menu(App *a, cairo_t *cr);

/* Returns the TOC entry index under window-coord (x,y), or -1. */
static int toc_index_at(App *a, int x, int y) {
    if (!a->toc_visible || x < 0 || x >= SIDEBAR_W) return -1;
    int y0 = TOC_PAD_TOP - (int)a->toc_scroll_y + 24;
    if (y < y0) return -1;
    int idx = (y - y0) / TOC_LINE_H;
    if (idx < 0 || idx >= (int)a->toc_entries.size()) return -1;
    return idx;
}

static void draw_status_bar(App *a, cairo_t *cr, int ww, int hh) {
    int top = hh - STATUS_H;
    if (top < 0) return;
    const Chrome &c = chrome(a);
    cairo_set_source_rgb(cr, c.bg[0], c.bg[1], c.bg[2]);
    cairo_rectangle(cr, 0, top, ww, STATUS_H); cairo_fill(cr);
    cairo_set_source_rgb(cr, c.sep[0], c.sep[1], c.sep[2]);
    cairo_rectangle(cr, 0, top, ww, 1); cairo_fill(cr);

    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(l, fd);

    cairo_set_source_rgb(cr, c.dim[0], c.dim[1], c.dim[2]);

    /* Left: filename */
    pango_layout_set_text(l, a->file_basename.c_str(), -1);
    int tw_l = 0, th_l = 0;
    pango_layout_get_pixel_size(l, &tw_l, &th_l);
    cairo_move_to(cr, 10, top + (STATUS_H - th_l) / 2.0);
    pango_cairo_show_layout(cr, l);

    /* Right: scroll % · zoom % (+ highlight indicator while jobs are running) */
    int max_y = std::max(1, (int)(a->doc_h * a->zoom) - a->vp_h);
    int scroll_pct = (a->doc_h > 0 && a->doc_h * a->zoom > a->vp_h)
                     ? (int)((a->scroll_y / max_y) * 100.0 + 0.5)
                     : 100;
    if (scroll_pct < 0)   scroll_pct = 0;
    if (scroll_pct > 100) scroll_pct = 100;
    int zoom_pct = (int)(a->zoom * 100.0 + 0.5);

    std::string right;
    if (a->highlighter.active() && !a->highlighter.all_done())
        right = "highlighting… · ";
    right += std::to_string(scroll_pct) + "%  · ";
    right += std::to_string(zoom_pct)   + "%";
    pango_layout_set_text(l, right.c_str(), -1);
    int rw = 0, rh = 0;
    pango_layout_get_pixel_size(l, &rw, &rh);
    cairo_move_to(cr, ww - rw - 10, top + (STATUS_H - rh) / 2.0);
    pango_cairo_show_layout(cr, l);

    pango_font_description_free(fd);
    g_object_unref(l);
}

static void draw_help_overlay(App *a, cairo_t *cr, int ww, int hh) {
    const Chrome &c = chrome(a);
    /* Translucent dim over the whole window. */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, a->dark_mode ? 0.55 : 0.40);
    cairo_rectangle(cr, 0, 0, ww, hh); cairo_fill(cr);

    static const char *lines[] = {
        "  Keyboard",
        "    j / ↓                 Scroll down",
        "    k / ↑                 Scroll up",
        "    Space / PageDown      Page down",
        "    Shift+Space / PgUp    Page up",
        "    g / Home              Top of document",
        "    G / End               Bottom of document",
        "    Ctrl++ / Ctrl+=       Zoom in",
        "    Ctrl+-                Zoom out",
        "    Ctrl+0                Reset zoom",
        "    Ctrl+T                Toggle outline sidebar",
        "    Ctrl+D                Toggle dark mode",
        "    Ctrl+C                Copy selection",
        "    Ctrl+A                Select all",
        "    Ctrl+F                Find in document",
        "      Enter / F3          Next match",
        "      Shift+Enter / S+F3  Previous match",
        "      Alt+C               Toggle case-sensitive",
        "      Alt+W               Toggle whole-word",
        "      Alt+R               Toggle regex",
        "      Ctrl+Backspace      Delete word",
        "      Esc                 Close find",
        "    F1 / ?                Toggle this help",
        "    q / Esc               Quit",
        "",
        "  Mouse",
        "    Wheel                 Scroll (sidebar scrolls outline)",
        "    Shift+Wheel           Horizontal scroll",
        "    Ctrl+Wheel            Zoom",
        "    Pinch                 Zoom (touchpad)",
        "    Click + drag          Select text",
        "    Double-click + drag   Select by word",
        "    Triple-click + drag   Select by line",
        "    Click on link         Follow link",
        "    Click outline entry   Jump to heading",
    };
    const int n_lines = (int)(sizeof(lines) / sizeof(lines[0]));

    /* Approximate panel dimensions; cap to fit viewport. */
    int panel_w = 540, panel_h = 24 + 18 * n_lines + 24;
    if (panel_w > ww - 40) panel_w = ww - 40;
    if (panel_h > hh - 40) panel_h = hh - 40;
    int x = (ww - panel_w) / 2, y = (hh - panel_h) / 2;

    /* Card. */
    cairo_set_source_rgba(cr, c.bg[0], c.bg[1], c.bg[2], 0.97);
    cairo_rectangle(cr, x, y, panel_w, panel_h); cairo_fill(cr);
    cairo_set_source_rgba(cr, c.sep[0], c.sep[1], c.sep[2], 1.0);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, panel_w - 1, panel_h - 1); cairo_stroke(cr);

    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Monospace 10");
    pango_layout_set_font_description(l, fd);
    cairo_set_source_rgb(cr, c.text[0], c.text[1], c.text[2]);
    int line_y = y + 18;
    for (int i = 0; i < n_lines; i++) {
        pango_layout_set_text(l, lines[i], -1);
        cairo_move_to(cr, x + 20, line_y);
        pango_cairo_show_layout(cr, l);
        line_y += 18;
    }
    pango_font_description_free(fd);
    g_object_unref(l);
}

/* Walk the litehtml tree looking for an element whose id="..." attribute
   matches. Returns null if not found. */
static litehtml::element::ptr find_by_id(litehtml::element::ptr el, const std::string &id) {
    if (!el) return nullptr;
    const char *cur = el->get_attr("id");
    if (cur && id == cur) return el;
    for (const auto &child : el->children()) {
        auto found = find_by_id(child, id);
        if (found) return found;
    }
    return nullptr;
}

static void scroll_to_anchor(App *a, const std::string &id) {
    if (!a->doc) return;
    auto el = find_by_id(a->doc->root(), id);
    if (!el) return;
    litehtml::position p = el->get_placement();
    a->scroll_y = (double)p.y * a->zoom;
    clamp(a);
}

/* Walk the litehtml tree looking for an <a> whose placement contains
   (doc_x, doc_y). For inline anchors, the placement only covers their
   primary line box — accuracy is limited but adequate for "is the right
   click on a link?" detection. Returns the href or empty string. */
static std::string href_at(litehtml::element::ptr el, int doc_x, int doc_y) {
    if (!el) return "";
    const char *tag = el->get_tagName();
    if (tag && (tag[0] == 'a' || tag[0] == 'A') && tag[1] == '\0') {
        litehtml::position p = el->get_placement();
        if (doc_x >= p.x && doc_x < p.x + p.width &&
            doc_y >= p.y && doc_y < p.y + p.height) {
            const char *href = el->get_attr("href");
            if (href) return href;
        }
    }
    for (const auto &c : el->children()) {
        std::string r = href_at(c, doc_x, doc_y);
        if (!r.empty()) return r;
    }
    return "";
}

/* Spawn xdg-open <url> detached. Double-fork pattern so the grandchild is
   re-parented to init and we don't need to waitpid it later. */
static void open_external_url(const std::string &url) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 == 0) {
            /* Detach from controlling terminal so xdg-open can manage its
               own children (browser etc.). */
            setsid();
            execlp("xdg-open", "xdg-open", url.c_str(), (char*)NULL);
            _exit(127);
        }
        _exit(0);
    }
    /* Parent: reap the immediate child to avoid a zombie. */
    waitpid(pid, NULL, 0);
}

static void draw_search_bar(App *a, cairo_t *cr, int ww) {
    int bh = SEARCH_BAR_H;
    const Chrome &c = chrome(a);

    cairo_set_source_rgba(cr, c.bg[0], c.bg[1], c.bg[2], 0.97);
    cairo_rectangle(cr, 0, 0, ww, bh); cairo_fill(cr);
    cairo_set_source_rgb(cr, c.sep[0], c.sep[1], c.sep[2]);
    cairo_rectangle(cr, 0, bh - 1, ww, 1); cairo_fill(cr);

    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 13");
    pango_layout_set_font_description(l, fd);

    /* "Find: <query>" with caret */
    std::string text = "Find: " + a->search_query;
    pango_layout_set_text(l, text.c_str(), -1);
    int tw_l = 0, th_l = 0;
    pango_layout_get_pixel_size(l, &tw_l, &th_l);
    cairo_set_source_rgb(cr, c.text[0], c.text[1], c.text[2]);
    double ty = (bh - th_l) / 2.0;
    cairo_move_to(cr, 12, ty);
    pango_cairo_show_layout(cr, l);
    cairo_rectangle(cr, 12 + tw_l + 1, ty, 2, th_l);
    cairo_fill(cr);

    /* Match count / hint (or red error tag for an invalid regex) */
    bool regex_bad = a->regex_mode && !a->search_query.empty()
                  && !CairoContainer::regex_is_valid(a->search_query, a->case_sensitive);
    std::string count;
    if (regex_bad)                           count = "Invalid regex";
    else if (a->search_query.empty())        count = "Enter · Esc · Alt+C/W/R · Ctrl+⌫";
    else if (a->search_matches.empty())      count = "No matches";
    else                                     count = std::to_string(a->current_match + 1) + " / " +
                                                     std::to_string(a->search_matches.size());
    pango_layout_set_text(l, count.c_str(), -1);
    int cnt_w = 0, cnt_h = 0;
    pango_layout_get_pixel_size(l, &cnt_w, &cnt_h);

    /* Three search-mode toggles, drawn right-to-left from just before the
       count: ".*" (regex), "Word" (whole word), "Aa" (case-sensitive). */
    auto draw_toggle = [&](const char *label, bool on, int x_right_edge,
                           int *out_box_w) {
        PangoLayout *li = pango_cairo_create_layout(cr);
        PangoFontDescription *fdi = pango_font_description_from_string("Sans Bold 11");
        pango_layout_set_font_description(li, fdi);
        pango_layout_set_text(li, label, -1);
        int iw = 0, ih = 0;
        pango_layout_get_pixel_size(li, &iw, &ih);
        int box_pad_x = 5, box_pad_y = 3;
        int box_w_ = iw + box_pad_x * 2;
        int box_h_ = ih + box_pad_y * 2;
        int x_box  = x_right_edge - box_w_;
        double y_box = (bh - box_h_) / 2.0;
        if (on) {
            cairo_set_source_rgba(cr, c.accent_bg[0], c.accent_bg[1], c.accent_bg[2], c.accent_bg[3]);
            cairo_rectangle(cr, x_box, y_box, box_w_, box_h_); cairo_fill(cr);
            cairo_set_source_rgba(cr, c.accent[0], c.accent[1], c.accent[2], 0.7);
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, x_box + 0.5, y_box + 0.5, box_w_ - 1, box_h_ - 1); cairo_stroke(cr);
            cairo_set_source_rgb(cr, c.accent[0], c.accent[1], c.accent[2]);
        } else {
            cairo_set_source_rgb(cr, c.dim[0], c.dim[1], c.dim[2]);
        }
        cairo_move_to(cr, x_box + box_pad_x, (bh - ih) / 2.0);
        pango_cairo_show_layout(cr, li);
        pango_font_description_free(fdi);
        g_object_unref(li);
        *out_box_w = box_w_;
    };

    int x_right = ww - 12;
    int x_count = x_right - cnt_w;
    int x_cursor = x_count - 10;
    int tw_box;
    draw_toggle(".*",   a->regex_mode,     x_cursor, &tw_box); x_cursor -= tw_box + 6;
    draw_toggle("Word", a->whole_word,     x_cursor, &tw_box); x_cursor -= tw_box + 6;
    draw_toggle("Aa",   a->case_sensitive, x_cursor, &tw_box);

    if (regex_bad) cairo_set_source_rgb(cr, 0.80, 0.20, 0.20);
    else           cairo_set_source_rgb(cr, c.dim[0], c.dim[1], c.dim[2]);
    cairo_move_to(cr, x_count, (bh - cnt_h) / 2.0);
    pango_cairo_show_layout(cr, l);

    pango_font_description_free(fd);
    g_object_unref(l);
}

/* ========================================================================= */
/* Right-click context menu                                                  */
/* ========================================================================= */

/* Forward declarations: menu_run calls these but they're defined further
   down in the file. */
static void toggle_dark_mode(App *a, mdv_window *w);
static void toggle_toc(App *a, mdv_window *w);
static void reload_file(void *ud);

static const int MENU_ITEM_H = 26;
static const int MENU_PAD_V  = 4;
static const int MENU_W      = 220;

static void menu_close(App *a) {
    a->menu.visible = false;
    a->menu.items.clear();
    a->menu.hover_idx = -1;
}

/* Build menu items based on right-click context: was the click over a link?
   does the selection contain anything? */
static void menu_build(App *a, int doc_x, int doc_y) {
    a->menu.items.clear();
    std::string href = href_at(a->doc ? a->doc->root() : nullptr, doc_x, doc_y);
    if (!href.empty() && href[0] != '#') {
        a->menu.items.push_back({"Open Link",     App::MenuAction::OpenLink, href, true});
        a->menu.items.push_back({"Copy Link URL", App::MenuAction::CopyLink, href, true});
    }
    bool has_selection = a->con.selection().active &&
                         !a->con.get_selected_text().empty();
    a->menu.items.push_back({"Copy",           App::MenuAction::Copy,       "", has_selection});
    a->menu.items.push_back({"Select All",     App::MenuAction::SelectAll,  "", a->doc != nullptr});
    a->menu.items.push_back({"Find…",          App::MenuAction::Find,       "", true});
    a->menu.items.push_back({"Reload",         App::MenuAction::Reload,     "", true});
    a->menu.items.push_back({a->dark_mode ? "Use Light Theme" : "Use Dark Theme",
                              App::MenuAction::ToggleDark, "", true});
}

/* Position the menu near (win_x, win_y), clamped so it stays in the window. */
static void menu_open_at(App *a, int win_x, int win_y, int ww) {
    if (a->menu.items.empty()) return;
    a->menu.w = MENU_W;
    a->menu.h = 2 * MENU_PAD_V + (int)a->menu.items.size() * MENU_ITEM_H;
    a->menu.x = win_x;
    a->menu.y = win_y;
    if (a->menu.x + a->menu.w > ww)         a->menu.x = ww - a->menu.w;
    if (a->menu.y + a->menu.h > a->vp_h)    a->menu.y = a->vp_h - a->menu.h;
    if (a->menu.x < 0)                      a->menu.x = 0;
    if (a->menu.y < 0)                      a->menu.y = 0;
    a->menu.visible = true;
    a->menu.hover_idx = -1;
}

static int menu_item_at(App *a, int x, int y) {
    if (!a->menu.visible) return -1;
    if (x < a->menu.x || x >= a->menu.x + a->menu.w) return -1;
    if (y < a->menu.y + MENU_PAD_V || y >= a->menu.y + a->menu.h - MENU_PAD_V) return -1;
    int idx = (y - a->menu.y - MENU_PAD_V) / MENU_ITEM_H;
    if (idx < 0 || idx >= (int)a->menu.items.size()) return -1;
    return idx;
}

static void draw_context_menu(App *a, cairo_t *cr) {
    const Chrome &c = chrome(a);

    /* Card with light shadow border. */
    cairo_set_source_rgba(cr, c.bg[0], c.bg[1], c.bg[2], 0.98);
    cairo_rectangle(cr, a->menu.x, a->menu.y, a->menu.w, a->menu.h); cairo_fill(cr);
    cairo_set_source_rgb(cr, c.sep[0], c.sep[1], c.sep[2]);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, a->menu.x + 0.5, a->menu.y + 0.5, a->menu.w - 1, a->menu.h - 1);
    cairo_stroke(cr);

    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 11");
    pango_layout_set_font_description(l, fd);

    for (size_t i = 0; i < a->menu.items.size(); i++) {
        const auto &it = a->menu.items[i];
        int row_y = a->menu.y + MENU_PAD_V + (int)i * MENU_ITEM_H;
        if ((int)i == a->menu.hover_idx && it.enabled) {
            cairo_set_source_rgba(cr, c.accent_bg[0], c.accent_bg[1], c.accent_bg[2], c.accent_bg[3]);
            cairo_rectangle(cr, a->menu.x + 2, row_y, a->menu.w - 4, MENU_ITEM_H);
            cairo_fill(cr);
        }
        pango_layout_set_text(l, it.label.c_str(), -1);
        int tw_l = 0, th_l = 0;
        pango_layout_get_pixel_size(l, &tw_l, &th_l);
        if (it.enabled) cairo_set_source_rgb(cr, c.text[0], c.text[1], c.text[2]);
        else            cairo_set_source_rgb(cr, c.dim[0],  c.dim[1],  c.dim[2]);
        cairo_move_to(cr, a->menu.x + 14, row_y + (MENU_ITEM_H - th_l) / 2.0);
        pango_cairo_show_layout(cr, l);
    }

    pango_font_description_free(fd);
    g_object_unref(l);
}

/* Execute a menu action. Window pointer needed for redraw / clipboard. */
static void menu_run(App *a, mdv_window *w, const App::MenuItem &it) {
    if (!it.enabled) return;
    switch (it.action) {
    case App::MenuAction::OpenLink:    open_external_url(it.param); break;
    case App::MenuAction::CopyLink:    mdv_window_copy_to_clipboard(w, it.param.c_str()); break;
    case App::MenuAction::Copy: {
        std::string t = a->con.get_selected_text();
        if (!t.empty()) mdv_window_copy_to_clipboard(w, t.c_str());
        break;
    }
    case App::MenuAction::SelectAll: {
        Selection &s = a->con.selection();
        s.anchor_x = 0; s.anchor_y = 0;
        s.extent_x = a->doc_w; s.extent_y = a->doc_h;
        s.active = true; s.dragging = false;
        break;
    }
    case App::MenuAction::Find:        open_search(a, w); break;
    case App::MenuAction::Reload:      reload_file(a);   break;
    case App::MenuAction::ToggleDark:  toggle_dark_mode(a, w); break;
    }
    mdv_window_request_redraw(w);
}

/* ========================================================================= */

static void on_redraw(mdv_window *w, cairo_t *cr, int ww, int hh, void *ud) {
    App *a = (App*)ud;
    /* Window-wide bg first (also paints behind the sidebar / status bar / search bar). */
    if (a->dark_mode) cairo_set_source_rgb(cr, 0.118, 0.118, 0.118);
    else              cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (!a->doc) return;

    int tw = toc_w(a);

    int dsx = (int)(a->scroll_x / a->zoom), dsy = (int)(a->scroll_y / a->zoom);
    a->con.set_scroll_offset(dsy);
    a->con.set_scroll_offset_x(dsx);
    a->con.begin_record();

    cairo_save(cr);
    /* Clip + translate so doc rendering happens in the doc area only
       (right of sidebar, above status bar). */
    cairo_rectangle(cr, tw, 0, a->vp_w, a->vp_h); cairo_clip(cr);
    cairo_translate(cr, tw, 0);
    cairo_scale(cr, a->zoom, a->zoom);

    int rw = (int)(a->vp_w / a->zoom), rh = (int)(a->vp_h / a->zoom);
    litehtml::position clip(0, 0, rw, rh);
    a->con.set_render_cr(cr);
    a->doc->draw((litehtml::uint_ptr)cr, -dsx, -dsy, &clip);
    a->con.set_render_cr(nullptr);

    /* Re-find matches against freshly-recorded spans if layout changed since last search */
    if (a->search_active && !a->search_query.empty() && a->match_count_dirty) {
        int prev_cm = a->current_match;
        a->search_matches = a->con.find_matches(a->search_query, a->case_sensitive, a->whole_word, a->regex_mode);
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

    /* Horizontal scrollbar — sits just above the status bar, not at the
       window bottom edge. Offset by sidebar width. */
    SBG sh = sbh(a);
    if (sh.vis) {
        double y = a->vp_h - SB_W;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.05);
        cairo_rectangle(cr, tw, y, sh.track, SB_W - 2); cairo_fill(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, a->sb_h_drag ? 0.45 : 0.25);
        cairo_rectangle(cr, tw + sh.pos, y, sh.len, SB_W - 2); cairo_fill(cr);
    }

    /* TOC sidebar (drawn over doc bg, to the left of doc area). */
    if (a->toc_visible) draw_sidebar(a, cr);

    /* Search bar overlay (drawn last, in viewport coords) */
    if (a->search_active) draw_search_bar(a, cr, ww);

    /* Status bar (always visible). */
    draw_status_bar(a, cr, ww, hh);

    /* Context menu (drawn just below the help overlay) */
    if (a->menu.visible) draw_context_menu(a, cr);

    /* Help overlay (drawn on top of everything). */
    if (a->help_visible) draw_help_overlay(a, cr, ww, hh);
}

static void on_resize(mdv_window *w, int ww, int hh, void *ud) {
    App *a = (App*)ud;
    a->vp_w = ww - toc_w(a);          /* sidebar steals from doc width */
    a->vp_h = hh - STATUS_H;          /* status bar always occupies bottom strip */
    if (a->vp_w < 0) a->vp_w = 0;
    if (a->vp_h < 0) a->vp_h = 0;
    do_layout(a); mdv_window_request_redraw(w);
}

static void on_scroll(mdv_window *w, int x, int y, double dx, double dy, uint32_t mods, void *ud) {
    App *a = (App*)ud;
    /* Wheel over the sidebar scrolls the TOC list, not the document. */
    if (a->toc_visible && x < SIDEBAR_W && y < a->vp_h) {
        a->toc_scroll_y += dy * 3.0;
        clamp_toc_scroll(a);
        mdv_window_request_redraw(w);
        return;
    }
    int tw = toc_w(a);
    int x_eff = x - tw;
    if (mods & MDV_MOD_CTRL) {
        apply_zoom(a, a->zoom + ((dy < 0) ? ZOOM_STEP : -ZOOM_STEP), x_eff, y);
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

/* Update the selection's anchor/extent during a drag, respecting granularity.
   For CHAR: the extent simply follows the cursor (existing behavior).
   For WORD/LINE: find the range under the cursor, then extend from the
   initial range — if the cursor's range is before the initial one, anchor
   to the initial range's end and extend to the cursor's start; if after,
   anchor to the initial's start and extend to the cursor's end; if
   overlapping (cursor still inside the initial range) keep the initial
   range exactly. Skip updates when the cursor lands somewhere with no
   word/line under it. */
static void extend_selection_drag(App *a, int doc_x, int doc_y) {
    Selection &sel = a->con.selection();
    if (sel.granularity == Selection::GRAN_CHAR) {
        sel.extent_x = doc_x;
        sel.extent_y = doc_y;
        sel.active = true;
        return;
    }
    TextRange curr = (sel.granularity == Selection::GRAN_WORD)
        ? a->con.find_word_at(doc_x, doc_y)
        : a->con.find_line_at(doc_x, doc_y);
    if (!curr.found) return;
    bool before_init = (curr.start_y < sel.init_start_y) ||
                       (curr.start_y == sel.init_start_y && curr.end_x <= sel.init_start_x);
    bool after_init  = (curr.end_y > sel.init_end_y) ||
                       (curr.end_y == sel.init_end_y && curr.start_x >= sel.init_end_x);
    if (before_init) {
        sel.anchor_x = sel.init_end_x;   sel.anchor_y = sel.init_end_y;
        sel.extent_x = curr.start_x;     sel.extent_y = curr.start_y;
    } else if (after_init) {
        sel.anchor_x = sel.init_start_x; sel.anchor_y = sel.init_start_y;
        sel.extent_x = curr.end_x;       sel.extent_y = curr.end_y;
    } else {
        sel.anchor_x = sel.init_start_x; sel.anchor_y = sel.init_start_y;
        sel.extent_x = sel.init_end_x;   sel.extent_y = sel.init_end_y;
    }
    sel.active = true;
}

static void on_mouse_move(mdv_window *w, int x, int y, void *ud) {
    App *a = (App*)ud;
    a->mouse_x = x; a->mouse_y = y;

    /* Context menu hover: when visible, takes over cursor handling. */
    if (a->menu.visible) {
        int idx = menu_item_at(a, x, y);
        if (idx != a->menu.hover_idx) {
            a->menu.hover_idx = idx;
            mdv_window_request_redraw(w);
        }
        mdv_window_set_cursor(w, "default");
        return;
    }

    /* Sidebar hover tracking */
    if (a->toc_visible && x < SIDEBAR_W && y < a->vp_h) {
        int idx = toc_index_at(a, x, y);
        if (idx != a->toc_hover_idx) {
            a->toc_hover_idx = idx;
            mdv_window_request_redraw(w);
        }
        mdv_window_set_cursor(w, "default");
        return;
    } else if (a->toc_hover_idx != -1) {
        a->toc_hover_idx = -1;
        mdv_window_request_redraw(w);
    }

    int tw = toc_w(a);
    int x_eff = x - tw;

    if (a->sb_v_drag) {
        SBG sv = sbv(a); double mt = a->vp_h - sv.len;
        if (mt > 0) { a->scroll_y = std::clamp((y - a->sb_drag_off) / mt, 0.0, 1.0) * max_sy(a); clamp(a); }
        mdv_window_request_redraw(w); return;
    }
    if (a->sb_h_drag) {
        SBG sh = sbh(a); double mt = a->vp_w - sh.len;
        if (mt > 0) { a->scroll_x = std::clamp((x_eff - a->sb_drag_off) / mt, 0.0, 1.0) * max_sx(a); clamp(a); }
        mdv_window_request_redraw(w); return;
    }

    /* Cursor: arrow over scrollbars, search bar, and status bar; text in doc. */
    { SBG sv = sbv(a); SBG sh = sbh(a);
      bool over_sb = (sv.vis && x_eff >= a->vp_w - SB_W)
                  || (sh.vis && y >= a->vp_h - SB_W && y < a->vp_h);
      bool over_search_bar = a->search_active && y < SEARCH_BAR_H;
      bool over_status_bar = y >= a->vp_h;
      mdv_window_set_cursor(w, (over_sb || over_search_bar || over_status_bar) ? "default" : "text"); }

    Selection &sel = a->con.selection();
    if (sel.dragging) {
        int dx2 = (int)((x_eff + a->scroll_x) / a->zoom);
        int dy2 = (int)((y + a->scroll_y) / a->zoom);
        extend_selection_drag(a, dx2, dy2);
        mdv_window_request_redraw(w); return;
    }

    if (!a->doc) return;
    int dx2 = (int)((x_eff + a->scroll_x) / a->zoom), dy2 = (int)((y + a->scroll_y) / a->zoom);
    std::vector<litehtml::position> rb;
    if (a->doc->on_mouse_over(dx2, dy2, dx2, dy2, rb)) mdv_window_request_redraw(w);
}

static void on_mouse_button(mdv_window *w, int x, int y, int btn, uint32_t mods, bool pr, int count, void *ud) {
    App *a = (App*)ud;
    int tw = toc_w(a);

    /* Context menu: while it's visible, every press anywhere is consumed by
       the menu — either running an item or dismissing. */
    if (a->menu.visible) {
        if (!pr) return;  /* releases pass through silently */
        int idx = menu_item_at(a, x, y);
        if (idx >= 0) {
            App::MenuItem it = a->menu.items[idx];
            menu_close(a);
            menu_run(a, w, it);
        } else {
            menu_close(a);
            mdv_window_request_redraw(w);
        }
        return;
    }

    /* Right-click in the doc area opens the menu. */
    if (btn == BTN_RIGHT && pr) {
        if (a->toc_visible && x < SIDEBAR_W) return;       /* not over sidebar */
        if (a->search_active && y < SEARCH_BAR_H) return;  /* not over search bar */
        if (y >= a->vp_h)                       return;    /* not over status bar */
        int x_eff_r = x - tw;
        int doc_x = (int)((x_eff_r + a->scroll_x) / a->zoom);
        int doc_y = (int)((y       + a->scroll_y) / a->zoom);
        menu_build(a, doc_x, doc_y);
        menu_open_at(a, x, y, mdv_window_width(w));
        mdv_window_request_redraw(w);
        return;
    }

    /* Sidebar: click on a TOC entry → scroll to anchor. */
    if (btn == BTN_LEFT && pr && a->toc_visible && x < SIDEBAR_W && y < a->vp_h) {
        int idx = toc_index_at(a, x, y);
        if (idx >= 0 && !a->toc_entries[idx].id.empty()) {
            scroll_to_anchor(a, a->toc_entries[idx].id);
            mdv_window_request_redraw(w);
        }
        return;
    }

    int x_eff = x - tw;

    if (btn == BTN_LEFT) {
        if (pr) {
            /* Vertical scrollbar */
            SBG sv = sbv(a);
            if (sv.vis && x_eff >= a->vp_w - SB_W) {
                if (y >= sv.pos && y <= sv.pos + sv.len) { a->sb_v_drag = true; a->sb_drag_off = y - sv.pos; }
                else { double mt = a->vp_h - sv.len; if (mt > 0) { a->scroll_y = std::clamp((y - sv.len/2.0) / mt, 0.0, 1.0) * max_sy(a); clamp(a); } }
                mdv_window_request_redraw(w); return;
            }
            /* Horizontal scrollbar (only the strip just above the status bar) */
            SBG sh = sbh(a);
            if (sh.vis && y >= a->vp_h - SB_W && y < a->vp_h) {
                if (x_eff >= sh.pos && x_eff <= sh.pos + sh.len) { a->sb_h_drag = true; a->sb_drag_off = x_eff - sh.pos; }
                else { double mt = a->vp_w - sh.len; if (mt > 0) { a->scroll_x = std::clamp((x_eff - sh.len/2.0) / mt, 0.0, 1.0) * max_sx(a); clamp(a); } }
                mdv_window_request_redraw(w); return;
            }
            /* Click in search bar: keep focus, don't start selection */
            if (a->search_active && y < SEARCH_BAR_H) return;
            /* Click in status bar: nothing (yet) */
            if (y >= a->vp_h) return;

            Selection &sel = a->con.selection();
            int dx = (int)((x_eff + a->scroll_x) / a->zoom), dy = (int)((y + a->scroll_y) / a->zoom);

            /* Double-click → word; triple-click → line. Shift falls through to
               char-level extend (the existing shift+click behavior). */
            if (count == 2 && !(mods & MDV_MOD_SHIFT)) {
                TextRange tr = a->con.find_word_at(dx, dy);
                if (tr.found) {
                    sel.granularity = Selection::GRAN_WORD;
                    sel.init_start_x = tr.start_x; sel.init_start_y = tr.start_y;
                    sel.init_end_x   = tr.end_x;   sel.init_end_y   = tr.end_y;
                    sel.anchor_x = tr.start_x; sel.anchor_y = tr.start_y;
                    sel.extent_x = tr.end_x;   sel.extent_y = tr.end_y;
                    sel.active = true; sel.dragging = true;
                    mdv_window_request_redraw(w);
                    return;
                }
            } else if (count == 3 && !(mods & MDV_MOD_SHIFT)) {
                TextRange tr = a->con.find_line_at(dx, dy);
                if (tr.found) {
                    sel.granularity = Selection::GRAN_LINE;
                    sel.init_start_x = tr.start_x; sel.init_start_y = tr.start_y;
                    sel.init_end_x   = tr.end_x;   sel.init_end_y   = tr.end_y;
                    sel.anchor_x = tr.start_x; sel.anchor_y = tr.start_y;
                    sel.extent_x = tr.end_x;   sel.extent_y = tr.end_y;
                    sel.active = true; sel.dragging = true;
                    mdv_window_request_redraw(w);
                    return;
                }
            }

            sel.granularity = Selection::GRAN_CHAR;
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
                /* Tiny char-level drags clear the selection (treats it as a
                   click, not a drag). Word/line selections are kept whole. */
                if (sel.granularity == Selection::GRAN_CHAR) {
                    int ddx = sel.anchor_x - sel.extent_x, ddy = sel.anchor_y - sel.extent_y;
                    if (ddx*ddx + ddy*ddy < 16) sel.clear();
                }
                mdv_window_request_redraw(w);
            }
        }
    }
    if (!a->doc) return;
    int dx = (int)((x_eff + a->scroll_x) / a->zoom), dy = (int)((y + a->scroll_y) / a->zoom);
    std::vector<litehtml::position> rb;
    if (pr) a->doc->on_lbutton_down(dx, dy, dx, dy, rb);
    else    a->doc->on_lbutton_up(dx, dy, dx, dy, rb);
}

static void on_pointer_leave(mdv_window *w, int lx, int ly, void *ud) {
    App *a = (App*)ud; Selection &sel = a->con.selection();
    if (!sel.dragging) return;
    a->ptr_outside = true; a->leave_x = (int)((lx - toc_w(a) + a->scroll_x) / a->zoom);
    a->autoscroll_dir = (ly < a->vp_h / 2) ? -1 : 1;
    mdv_window_set_tick_enabled(w, true);
}

static void on_pointer_enter(mdv_window *w, int x, int y, void *ud) {
    App *a = (App*)ud; a->ptr_outside = false; a->autoscroll_dir = 0;
    mdv_window_set_tick_enabled(w, tick_on(a));
    Selection &sel = a->con.selection();
    if (sel.dragging) {
        int dx2 = (int)((x - toc_w(a) + a->scroll_x) / a->zoom);
        int dy2 = (int)((y + a->scroll_y) / a->zoom);
        extend_selection_drag(a, dx2, dy2);
        mdv_window_request_redraw(w);
    }
}

static void on_pinch(mdv_window *w, double scale, int cx, int cy, bool finished, void *ud) {
    App *a = (App*)ud;
    if (finished) return;
    if (std::abs(scale - 1.0) < 0.001) { a->pinch_base_zoom = a->zoom; return; }
    apply_zoom(a, a->pinch_base_zoom * scale, cx - toc_w(a), cy);
    mdv_window_request_redraw(w);
}

static void on_tick(mdv_window *w, void *ud) {
    App *a = (App*)ud; bool dirty = false;
    Selection &sel = a->con.selection();
    if (sel.dragging && a->autoscroll_dir != 0) {
        a->scroll_y += a->autoscroll_dir * AUTOSCROLL_SPD; clamp(a);
        int target_y = (a->autoscroll_dir < 0) ? (int)(a->scroll_y / a->zoom)
                                               : (int)((a->scroll_y + a->vp_h) / a->zoom);
        extend_selection_drag(a, a->leave_x, target_y);
        dirty = true;
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

/* Ctrl+Backspace style word delete: skip trailing whitespace, then delete
   back to the next whitespace boundary (or start of string). */
static void delete_word_back(std::string &s) {
    if (s.empty()) return;
    size_t i = s.size();
    while (i > 0 && isspace((unsigned char)s[i - 1])) i--;
    while (i > 0 && !isspace((unsigned char)s[i - 1])) i--;
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
                if (mods & MDV_MOD_CTRL) delete_word_back(a->search_query);
                else                     utf8_pop_back(a->search_query);
                a->match_count_dirty = true;
                if (a->search_query.empty()) {
                    a->search_matches.clear();
                    a->current_match = -1;
                }
                mdv_window_request_redraw(w);
            }
            return;
        }
        if (mods & MDV_MOD_ALT) {
            if      (ks == XKB_KEY_c) { a->case_sensitive = !a->case_sensitive; a->match_count_dirty = true; mdv_window_request_redraw(w); }
            else if (ks == XKB_KEY_w) { a->whole_word     = !a->whole_word;     a->match_count_dirty = true; mdv_window_request_redraw(w); }
            else if (ks == XKB_KEY_r) { a->regex_mode     = !a->regex_mode;     a->match_count_dirty = true; mdv_window_request_redraw(w); }
            /* Eat all other Alt combos so they don't get typed as text. */
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
        if (ks == XKB_KEY_equal || ks == XKB_KEY_plus) { apply_zoom(a, a->zoom + ZOOM_STEP, a->mouse_x - toc_w(a), a->mouse_y); mdv_window_request_redraw(w); return; }
        if (ks == XKB_KEY_minus) { apply_zoom(a, a->zoom - ZOOM_STEP, a->mouse_x - toc_w(a), a->mouse_y); mdv_window_request_redraw(w); return; }
        if (ks == XKB_KEY_0)     { apply_zoom(a, 1.0, a->mouse_x - toc_w(a), a->mouse_y); mdv_window_request_redraw(w); return; }
        if (ks == XKB_KEY_t)     { toggle_toc(a, w); return; }
        if (ks == XKB_KEY_d)     { toggle_dark_mode(a, w); return; }
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
    case XKB_KEY_F1: case XKB_KEY_question:
        a->help_visible = !a->help_visible;
        mdv_window_request_redraw(w);
        return;
    case XKB_KEY_Escape:
        if (a->menu.visible) { menu_close(a); mdv_window_request_redraw(w); return; }
        if (a->help_visible) { a->help_visible = false; mdv_window_request_redraw(w); return; }
        mdv_window_quit(w); return;
    case XKB_KEY_q:                      mdv_window_quit(w); return;
    }
}

static void on_close(mdv_window *w, void *ud) { (void)ud; mdv_window_quit(w); }

/* Re-create the litehtml document with whatever the highlighter has
   produced so far, preserving scroll position. Called from the highlight
   milestone callback after the first batch finishes and again when all
   blocks have completed. */
static void rebuild_doc_with_highlight(App *a, mdv_window *w) {
    std::string new_html = a->highlighter.build_html(a->original_html);
    if (new_html == a->html) return;  /* nothing to do */
    a->html = std::move(new_html);

    double sx = a->scroll_x, sy = a->scroll_y;
    a->doc = litehtml::document::createFromString(a->html.c_str(), &a->con, litehtml::master_css);
    do_layout(a);
    a->scroll_x = sx; a->scroll_y = sy;
    clamp(a);
    a->match_count_dirty = true;
    mdv_window_request_redraw(w);
}

static void on_highlight_milestone(mdv_window *w, void *ud) {
    rebuild_doc_with_highlight((App*)ud, w);
}

static void start_highlight(App *a, mdv_window *w) {
    if (!a->highlighter.active()) return;
    auto blocks = mdv::extract_code_blocks(a->original_html);
    if (blocks.empty()) return;
    a->highlighter.start(w, std::move(blocks), on_highlight_milestone, a);
}

/* Re-render the document from cached source markdown using whichever theme
   is currently active. Called from the dark-mode toggle (and indirectly
   from reload, which has its own re-read flow). Preserves scroll. */
static void rerender_doc(App *a) {
    a->highlighter.stop();
    a->con.set_dark_mode(a->dark_mode);
    a->html          = add_heading_ids(md2html(a->source_md, a->dark_mode));
    a->original_html = a->html;
    a->toc_entries   = extract_toc(a->original_html);
    clamp_toc_scroll(a);

    double sx = a->scroll_x, sy = a->scroll_y;
    a->doc = litehtml::document::createFromString(a->html.c_str(), &a->con, litehtml::master_css);
    do_layout(a);
    a->scroll_x = sx; a->scroll_y = sy; clamp(a);
    a->match_count_dirty = true;
}

static void toggle_dark_mode(App *a, mdv_window *w) {
    a->dark_mode = !a->dark_mode;
    rerender_doc(a);
    if (a->highlighter.tool() != mdv::HLTool::None) start_highlight(a, w);
    mdv_window_request_redraw(w);
}

static void toggle_toc(App *a, mdv_window *w) {
    a->toc_visible = !a->toc_visible;
    /* Recompute vp_w based on the current window dimensions. */
    a->vp_w = mdv_window_width(w) - toc_w(a);
    if (a->vp_w < 0) a->vp_w = 0;
    if (a->toc_visible) a->toc_entries = extract_toc(a->original_html);
    do_layout(a);
    mdv_window_request_redraw(w);
}

static void on_first_frame(mdv_window *w, void *ud) {
    start_highlight((App*)ud, w);
}

/* Re-read the source file from disk and rebuild everything. Called by
   LiveReloader when the file changes on disk. Preserves scroll, search
   state, selection. */
static void reload_file(void *ud) {
    App *a = (App*)ud;
    std::ifstream f(a->file_path);
    if (!f.is_open()) return;
    std::ostringstream ss; ss << f.rdbuf(); f.close();
    std::string new_md = ss.str();
    if (new_md == a->source_md) return;  /* same content, no change */
    a->source_md = std::move(new_md);

    rerender_doc(a);
    if (a->highlighter.tool() != mdv::HLTool::None) start_highlight(a, a->win);

    mdv_window_request_redraw(a->win);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <path-to-markdown-file>\n"
        "\n"
        "Options:\n"
        "  --no-highlight   disable syntax highlighting of code blocks\n"
        "  --no-reload      disable live-reload on file change\n"
        "  -h, --help       show this help\n"
        "\n"
        "Env: MDVIEW_NO_HIGHLIGHT also disables highlighting.\n",
        prog);
}

int main(int argc, char **argv) {
    bool no_highlight = false;
    bool no_reload    = false;
    const char *file = nullptr;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--no-highlight") == 0) no_highlight = true;
        else if (strcmp(argv[i], "--no-reload")    == 0) no_reload = true;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]); return 0;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]); return 1;
        }
        else if (!file) file = argv[i];
        else {
            fprintf(stderr, "Extra argument: %s\n", argv[i]);
            print_usage(argv[0]); return 1;
        }
    }
    if (!file) { print_usage(argv[0]); return 1; }

    std::ifstream f(file);
    if (!f.is_open()) { fprintf(stderr, "Error: cannot open '%s'\n", file); return 1; }
    std::ostringstream ss; ss << f.rdbuf(); f.close();

    mdv::State persisted = mdv::load_state();

    App app;
    app.file_path = file;
    { auto sl = app.file_path.find_last_of('/');
      app.file_basename = (sl == std::string::npos) ? app.file_path : app.file_path.substr(sl + 1); }
    app.zoom            = persisted.zoom;
    app.dark_mode       = persisted.dark_mode;
    app.case_sensitive  = persisted.case_sensitive;
    app.whole_word      = persisted.whole_word;
    app.regex_mode      = persisted.regex_mode;
    app.source_md       = ss.str();
    app.html            = add_heading_ids(md2html(app.source_md, app.dark_mode));
    app.original_html   = app.html;
    app.toc_entries     = extract_toc(app.original_html);
    app.con.set_dark_mode(app.dark_mode);
    { auto sl = app.file_path.find_last_of('/');
      app.con.set_base_dir(sl != std::string::npos ? app.file_path.substr(0, sl) : "."); }
    app.doc = litehtml::document::createFromString(app.html.c_str(), &app.con, litehtml::master_css);
    app.pinch_base_zoom = 1.0;

    app.highlighter.set_tool(mdv::detect_tool(no_highlight));

    /* Anchor clicks: # → in-doc scroll; everything else → xdg-open. */
    app.con.set_anchor_click_cb([&app](const std::string &url) {
        if (url.empty()) return;
        if (url[0] == '#') {
            scroll_to_anchor(&app, url.substr(1));
            if (app.win) mdv_window_request_redraw(app.win);
        } else {
            open_external_url(url);
        }
    });

    mdv_callbacks cb = {};
    cb.on_redraw = on_redraw; cb.on_resize = on_resize; cb.on_scroll = on_scroll;
    cb.on_mouse_move = on_mouse_move; cb.on_mouse_button = on_mouse_button;
    cb.on_key = on_key; cb.on_close = on_close;
    cb.on_pointer_leave = on_pointer_leave; cb.on_pointer_enter = on_pointer_enter;
    cb.on_tick = on_tick; cb.on_pinch = on_pinch;
    cb.userdata = &app;

    int win_w = std::max(200, persisted.window_width);
    int win_h = std::max(200, persisted.window_height);
    std::string title = std::string("mdview - ") + file;
    mdv_window *w = mdv_window_create(title.c_str(), win_w, win_h, &cb);
    if (!w) { fprintf(stderr, "Error: cannot create Wayland window\n"); return 1; }
    app.win = w;
    do_layout(&app);

    mdv_window_set_after_init(w, on_first_frame, &app);
    if (!no_reload) app.live_reloader.start(w, app.file_path, reload_file, &app);

    mdv_window_run(w);

    /* Snapshot state before tearing down. */
    mdv::State out;
    out.zoom           = app.zoom;
    out.window_width   = mdv_window_surface_width(w);
    out.window_height  = mdv_window_surface_height(w);
    out.dark_mode      = app.dark_mode;
    out.case_sensitive = app.case_sensitive;
    out.whole_word     = app.whole_word;
    out.regex_mode     = app.regex_mode;
    mdv::save_state(out);

    app.live_reloader.stop();
    app.highlighter.stop();
    mdv_window_destroy(w);
    return 0;
}

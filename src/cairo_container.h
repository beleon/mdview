#pragma once

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <librsvg/rsvg.h>

#include "litehtml.h"

#include <cmath>
#include <limits>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <cctype>
#include <functional>
#include <regex>

struct CairoFont {
    PangoFontDescription *desc;
    bool underline, strikethrough;
    int ascent, descent, height, x_height;
};

struct TextSpan {
    int x, y, width, height;
    std::string text;
    PangoFontDescription *font_desc;
};

struct Selection {
    enum Granularity { GRAN_CHAR, GRAN_WORD, GRAN_LINE };

    bool active = false, dragging = false;
    int anchor_x = 0, anchor_y = 0, extent_x = 0, extent_y = 0;

    /* Granularity of the current drag (set on press, used on motion+release). */
    Granularity granularity = GRAN_CHAR;
    /* For GRAN_WORD / GRAN_LINE: the range under the initial click. Drags
       extend the selection around this range rather than collapsing to a
       single character position. */
    int init_start_x = 0, init_start_y = 0;
    int init_end_x = 0,   init_end_y = 0;

    void get_ordered(int &sx, int &sy, int &ex, int &ey) const {
        if (anchor_y < extent_y || (anchor_y == extent_y && anchor_x <= extent_x))
            { sx = anchor_x; sy = anchor_y; ex = extent_x; ey = extent_y; }
        else
            { sx = extent_x; sy = extent_y; ex = anchor_x; ey = anchor_y; }
    }
    void clear() { active = false; dragging = false; granularity = GRAN_CHAR; }
};

struct SearchHit {
    int doc_x, doc_y, width, height;
};

struct TextRange {
    bool found = false;
    int start_x = 0, start_y = 0;  /* doc coords; y is line center */
    int end_x = 0,   end_y = 0;
};

struct CachedImage {
    cairo_surface_t *surface = nullptr;
    RsvgHandle      *svg     = nullptr;
    int width = 0, height = 0;
    ~CachedImage() {
        if (surface) cairo_surface_destroy(surface);
        if (svg)     g_object_unref(svg);
    }
    bool is_svg() const { return svg != nullptr; }
    void render_svg(cairo_t *cr, double tw, double th) const {
        if (!svg) return;
        RsvgRectangle vp = { 0, 0, tw, th };
        rsvg_handle_render_document(svg, cr, &vp, NULL);
    }
    static CachedImage *load(const std::string &path) {
        { auto dot = path.rfind('.');
          if (dot != std::string::npos) {
            std::string ext = path.substr(dot);
            for (auto &c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".svg" || ext == ".svgz") {
                GError *err = nullptr;
                RsvgHandle *h = rsvg_handle_new_from_file(path.c_str(), &err);
                if (h) {
                    double w=0,h2=0; bool got=false;
                    if (rsvg_handle_get_intrinsic_size_in_pixels(h,&w,&h2)&&w>0&&h2>0) got=true;
                    if (!got) { gboolean hw,hh,hv; RsvgLength rw,rh; RsvgRectangle vb;
                        rsvg_handle_get_intrinsic_dimensions(h,&hw,&rw,&hh,&rh,&hv,&vb);
                        if(hv&&vb.width>0&&vb.height>0){w=vb.width;h2=vb.height;got=true;} }
                    if (got) { auto *ci=new CachedImage(); ci->svg=h; ci->width=(int)ceil(w); ci->height=(int)ceil(h2); return ci; }
                    g_object_unref(h);
                }
                if(err)g_error_free(err);
            }
          }
        }
        GError *err=nullptr;
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(path.c_str(), &err);
        if (!pb) { if(err)g_error_free(err); return nullptr; }
        int w=gdk_pixbuf_get_width(pb), h=gdk_pixbuf_get_height(pb);
        bool alpha=gdk_pixbuf_get_has_alpha(pb); int rs=gdk_pixbuf_get_rowstride(pb);
        const guchar *px=gdk_pixbuf_get_pixels(pb);
        cairo_surface_t *sf=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);
        unsigned char *dst=cairo_image_surface_get_data(sf); int ds=cairo_image_surface_get_stride(sf);
        cairo_surface_flush(sf);
        for(int y=0;y<h;y++){const guchar*sr=px+y*rs;uint32_t*dr=(uint32_t*)(dst+y*ds);
            for(int x=0;x<w;x++){unsigned r=sr[0],g=sr[1],b=sr[2],a=alpha?sr[3]:255;
                r=r*a/255;g=g*a/255;b=b*a/255;dr[x]=(a<<24)|(r<<16)|(g<<8)|b;sr+=alpha?4:3;}}
        cairo_surface_mark_dirty(sf); g_object_unref(pb);
        auto *ci=new CachedImage(); ci->surface=sf; ci->width=w; ci->height=h; return ci;
    }
};

class CairoContainer : public litehtml::document_container {
public:
    CairoContainer() {
        ss_=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1,1);
        sc_=cairo_create(ss_);
        /* Disable hint_metrics so glyph widths stay at fractional precision.
           With hinting on, widths snap to whole pixels — fine at zoom=1.0
           but every fractional zoom step then has to redistribute the
           rounding error across the line, which makes monospace columns
           wobble (and occasionally overlap) under pinch zoom. */
        cairo_font_options_t *fo = cairo_font_options_create();
        cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
        cairo_set_font_options(sc_, fo);
        cairo_font_options_destroy(fo);
        pl_=pango_cairo_create_layout(sc_);
    }
    ~CairoContainer() { g_object_unref(pl_); cairo_destroy(sc_); cairo_surface_destroy(ss_);
        for(auto &p:imgs_) delete p.second; }

    void set_viewport(int w,int h){vw_=w;vh_=h;}
    void set_scroll_offset(int sy){sy_=sy;} void set_scroll_offset_x(int sx){sx_=sx;}
    void set_base_dir(const std::string &d){bd_=d;}
    void set_render_cr(cairo_t *cr){rcr_=cr;}
    void begin_record(){spans_.clear();}
    const std::vector<TextSpan>&spans()const{return spans_;}
    Selection&selection(){return sel_;}

    std::string resolve_path(const std::string &s)const{
        if(s.empty()||s[0]=='/')return s; return bd_.empty()?s:bd_+"/"+s; }
    CachedImage*get_or_load(const std::string &s){
        auto it=imgs_.find(s);if(it!=imgs_.end())return it->second;
        auto*i=CachedImage::load(resolve_path(s));imgs_[s]=i;return i; }

    int x_to_index(const TextSpan &sp,int dx)const{
        int lx=dx-sp.x; if(lx<=0)return 0; if(lx>=sp.width)return(int)sp.text.size();
        pango_layout_set_font_description(pl_,sp.font_desc); pango_layout_set_text(pl_,sp.text.c_str(),-1);
        int idx=0,tr=0; pango_layout_xy_to_index(pl_,lx*PANGO_SCALE,0,&idx,&tr);
        if(tr>0){const char*p=sp.text.c_str()+idx;idx+=(int)(g_utf8_next_char(p)-p);}
        return std::clamp(idx,0,(int)sp.text.size()); }
    int index_to_x(const TextSpan &sp,int bi)const{
        if(bi<=0)return sp.x; if(bi>=(int)sp.text.size())return sp.x+sp.width;
        pango_layout_set_font_description(pl_,sp.font_desc); pango_layout_set_text(pl_,sp.text.c_str(),-1);
        PangoRectangle pos; pango_layout_index_to_pos(pl_,bi,&pos); return sp.x+pos.x/PANGO_SCALE; }

    struct LineGroup{int start,end,min_top,max_bot;};

public:
    /* Snap an arbitrary doc-y to the center of the nearest recorded line.
       Used to enforce that selection endpoints always live on some line.

       Optional `bias_y`: if the input y is within SNAP_BIAS_PX of bias_y,
       return bias_y itself. Lets callers say "stick to whatever line this
       OTHER y is on, as long as we haven't moved meaningfully away" —
       used by extend_selection_drag with bias_y = anchor's y so a few
       pixels of vertical wobble during a horizontal drag don't kick the
       extent to a neighbouring line. */
    static constexpr int SNAP_BIAS_PX = 12;
    int snap_y_to_line(int y, int bias_y = std::numeric_limits<int>::min()) const {
        if (spans_.empty()) return y;
        if (bias_y != std::numeric_limits<int>::min()
         && std::abs(y - bias_y) <= SNAP_BIAS_PX) {
            return bias_y;
        }
        for (const TextSpan &sp : spans_) {
            if (y >= sp.y && y < sp.y + sp.height) return sp.y + sp.height / 2;
        }
        int best_cy = y;
        int best_dist = std::numeric_limits<int>::max();
        for (const TextSpan &sp : spans_) {
            int top = sp.y, bot = sp.y + sp.height;
            int dist = (y < top) ? (top - y) : (y - bot + 1);
            if (dist < best_dist) {
                best_dist = dist;
                best_cy   = sp.y + sp.height / 2;
            }
        }
        return best_cy;
    }

    /* Left/right pixel bounds of the line whose y-center is `lcy`,
       computed from ALL spans on that line (not just the selected
       subset). Used to clamp the selection rectangle so it doesn't
       extend into the line's margins. */
    int line_bounds_left(int lcy) const {
        int best = std::numeric_limits<int>::max();
        for (const TextSpan &sp : spans_) {
            int cy = sp.y + sp.height / 2;
            if (std::abs(cy - lcy) <= 4 && sp.x < best) best = sp.x;
        }
        return best;
    }
    int line_bounds_right(int lcy) const {
        int best = std::numeric_limits<int>::min();
        for (const TextSpan &sp : spans_) {
            int cy = sp.y + sp.height / 2;
            int r  = sp.x + sp.width;
            if (std::abs(cy - lcy) <= 4 && r > best) best = r;
        }
        return best;
    }
    /* Like sel_.get_ordered but with sy/ey snapped to the closest line. */
    void selection_bounds(int &sx, int &sy, int &ex, int &ey) const {
        sel_.get_ordered(sx, sy, ex, ey);
        sy = snap_y_to_line(sy);
        ey = snap_y_to_line(ey);
    }

    std::vector<const TextSpan*>get_selected_sorted()const{
        if(!sel_.active)return{}; int sx,sy,ex,ey;selection_bounds(sx,sy,ex,ey);
        std::vector<const TextSpan*>s; for(auto&sp:spans_)if(span_in(sp,sx,sy,ex,ey))s.push_back(&sp);
        std::sort(s.begin(),s.end(),[](const TextSpan*a,const TextSpan*b){
            int ay=a->y+a->height/2,by=b->y+b->height/2;return(std::abs(ay-by)>4)?ay<by:a->x<b->x;}); return s; }
    std::vector<LineGroup>group_lines(const std::vector<const TextSpan*>&s)const{
        std::vector<LineGroup>lines;int i=0,n=(int)s.size();
        while(i<n){int ly=s[i]->y+s[i]->height/2;LineGroup lg;lg.start=i;lg.min_top=s[i]->y;lg.max_bot=s[i]->y+s[i]->height;i++;
            while(i<n&&std::abs((s[i]->y+s[i]->height/2)-ly)<=4){lg.min_top=std::min(lg.min_top,s[i]->y);lg.max_bot=std::max(lg.max_bot,s[i]->y+s[i]->height);i++;}
            lg.end=i;lines.push_back(lg);}return lines;}
    /* clip_left: where the selection visually STARTS on its first line.
         - rx inside a span → snap to a character boundary.
         - rx in an inter-span gap or in the line's left margin → return
           rx itself, so the highlight begins at the click/cursor x. (No
           clamping — if you click in a margin and drag right, the rect
           starts at your click, not at the first word's left edge.)
       clip_right: where the selection visually ENDS on its last line.
         - rx inside a span → snap to a character boundary.
         - rx in an inter-span gap → return rx.
         - rx past the rightmost text on the line → clamp to that
           rightmost text. Extending the highlight rectangle into the
           empty right margin past the last word looked weird. */
    int clip_left(const std::vector<const TextSpan*>&s,const LineGroup&lg,int rx)const{
        for(int j=lg.start;j<lg.end;j++){
            if(rx>=s[j]->x&&rx<=s[j]->x+s[j]->width)return index_to_x(*s[j],x_to_index(*s[j],rx));
            if(rx<s[j]->x)return rx;
        }
        return s[lg.start]->x;
    }
    int clip_right(const std::vector<const TextSpan*>&s,const LineGroup&lg,int rx)const{
        int lcy   = s[lg.start]->y + s[lg.start]->height / 2;
        int right = line_bounds_right(lcy);
        if (right != std::numeric_limits<int>::min() && rx > right) rx = right;
        for(int j=lg.end-1;j>=lg.start;j--){
            if(rx>=s[j]->x&&rx<=s[j]->x+s[j]->width)return index_to_x(*s[j],x_to_index(*s[j],rx));
            if(rx>s[j]->x+s[j]->width)return rx;
        }
        return s[lg.end-1]->x+s[lg.end-1]->width;
    }

    /* Find the recorded text span whose bounding box contains (doc_x, doc_y).
       Linear scan — fine for typical document sizes. */
    const TextSpan *find_span_at(int doc_x, int doc_y) const {
        for (const auto &sp : spans_) {
            if (doc_y >= sp.y && doc_y < sp.y + sp.height &&
                doc_x >= sp.x && doc_x < sp.x + sp.width)
                return &sp;
        }
        return nullptr;
    }

    /* Returns the word boundary at (doc_x, doc_y). Matches browser behavior:
       characters fall into three classes — Word (alphanumeric / underscore /
       non-ASCII), Punct (everything else printable), and Space. A "word" is
       a maximal run of the SAME class within a single TextSpan, so
       double-clicking "hello," on the comma selects "," and double-clicking
       on "hello" selects "hello". Lands on Space → not found. */
    TextRange find_word_at(int doc_x, int doc_y) const {
        TextRange r;
        const TextSpan *sp = find_span_at(doc_x, doc_y);
        if (!sp) return r;
        const std::string &t = sp->text;
        int n = (int)t.size();
        if (n == 0) return r;
        int idx = x_to_index(*sp, doc_x);
        if (idx >= n) idx = n - 1;
        if (idx < 0) idx = 0;
        /* Align idx to a codepoint leading byte. */
        while (idx > 0 && (((unsigned char)t[idx]) & 0xC0) == 0x80) idx--;

        CharClass cls = classify((unsigned char)t[idx]);
        if (cls == CharClass::Space) return r;

        int start = idx;
        while (start > 0) {
            int prev = start - 1;
            while (prev > 0 && (((unsigned char)t[prev]) & 0xC0) == 0x80) prev--;
            if (classify((unsigned char)t[prev]) != cls) break;
            start = prev;
        }
        int end = idx;
        while (end < n) {
            if (classify((unsigned char)t[end]) != cls) break;
            int next = end + 1;
            while (next < n && (((unsigned char)t[next]) & 0xC0) == 0x80) next++;
            end = next;
        }
        if (start >= end) return r;
        r.found = true;
        r.start_x = index_to_x(*sp, start);
        r.end_x   = index_to_x(*sp, end);
        r.start_y = sp->y + sp->height / 2;
        r.end_y   = r.start_y;
        return r;
    }

    /* Returns the full-line range at (doc_x, doc_y): the union of all spans
       sharing the same y-band (center within 4px), spanning from the
       leftmost x to the rightmost x. */
    TextRange find_line_at(int doc_x, int doc_y) const {
        TextRange r;
        const TextSpan *sp = find_span_at(doc_x, doc_y);
        if (!sp) {
            /* Clicked between spans on a line — find by y only. */
            for (const auto &sp2 : spans_) {
                if (doc_y >= sp2.y && doc_y < sp2.y + sp2.height) { sp = &sp2; break; }
            }
            if (!sp) return r;
        }
        int ly = sp->y + sp->height / 2;
        int min_x = sp->x, max_x = sp->x + sp->width;
        for (const auto &sp2 : spans_) {
            int cy = sp2.y + sp2.height / 2;
            if (std::abs(cy - ly) <= 4) {
                if (sp2.x < min_x) min_x = sp2.x;
                if (sp2.x + sp2.width > max_x) max_x = sp2.x + sp2.width;
            }
        }
        r.found = true;
        r.start_x = min_x; r.end_x = max_x;
        r.start_y = ly;    r.end_y = ly;
        return r;
    }

    /* Substring search over recorded TextSpans.
       - case_sensitive: when false, query and span text are lowercased (ASCII).
       - whole_word: match must have a non-Word char (or string edge) on both sides.
       - regex_mode: query is an ECMAScript regex (case-insensitivity via regex::icase).
         Invalid patterns return an empty result (UI surfaces this via a red tint).
       Per-span search — multi-line or cross-formatting matches still won't work. */
    std::vector<SearchHit> find_matches(const std::string &query,
                                        bool case_sensitive,
                                        bool whole_word,
                                        bool regex_mode) const {
        std::vector<SearchHit> r;
        if (query.empty() || spans_.empty()) return r;

        auto check_word_boundary = [&](const std::string &t, size_t start, size_t len) {
            if (!whole_word) return true;
            bool ok_before = (start == 0)
                          || classify((unsigned char)t[start - 1]) != CharClass::Word;
            bool ok_after  = (start + len >= t.size())
                          || classify((unsigned char)t[start + len]) != CharClass::Word;
            return ok_before && ok_after;
        };
        auto record_hit = [&](const TextSpan &sp, size_t start, size_t len) {
            int x1 = index_to_x(sp, (int)start);
            int x2 = index_to_x(sp, (int)(start + len));
            r.push_back({x1, sp.y, x2 - x1, sp.height});
        };

        if (regex_mode) {
            std::regex::flag_type flags = std::regex::ECMAScript;
            if (!case_sensitive) flags |= std::regex::icase;
            std::regex re;
            try { re = std::regex(query, flags); }
            catch (const std::regex_error &) { return r; }
            for (const TextSpan &sp : spans_) {
                try {
                    auto begin = std::sregex_iterator(sp.text.begin(), sp.text.end(), re);
                    auto end   = std::sregex_iterator();
                    for (auto it = begin; it != end; ++it) {
                        auto m = *it;
                        size_t start = (size_t)m.position(0);
                        size_t len   = (size_t)m.length(0);
                        if (len == 0) continue;            /* skip zero-width matches */
                        if (!check_word_boundary(sp.text, start, len)) continue;
                        record_hit(sp, start, len);
                    }
                } catch (const std::regex_error &) { /* ignore per-span errors */ }
            }
        } else {
            std::string q = query;
            if (!case_sensitive) for (auto &c : q) c = (char)tolower((unsigned char)c);
            for (const TextSpan &sp : spans_) {
                std::string t = sp.text;
                if (!case_sensitive) for (auto &c : t) c = (char)tolower((unsigned char)c);
                size_t pos = 0;
                while ((pos = t.find(q, pos)) != std::string::npos) {
                    if (check_word_boundary(t, pos, q.size()))
                        record_hit(sp, pos, q.size());
                    pos += q.size();
                }
            }
        }

        std::sort(r.begin(), r.end(), [](const SearchHit &a, const SearchHit &b) {
            int ay = a.doc_y + a.height / 2, by = b.doc_y + b.height / 2;
            if (std::abs(ay - by) > 4) return ay < by;
            return a.doc_x < b.doc_x;
        });
        return r;
    }

    /* Quick syntax check for a regex pattern in the same flavor as
       find_matches. Used by the UI to flag invalid patterns visually. */
    static bool regex_is_valid(const std::string &q, bool case_sensitive) {
        if (q.empty()) return true;
        std::regex::flag_type flags = std::regex::ECMAScript;
        if (!case_sensitive) flags |= std::regex::icase;
        try { std::regex re(q, flags); (void)re; return true; }
        catch (const std::regex_error &) { return false; }
    }

    void draw_search_matches(cairo_t *cr,const std::vector<SearchHit>&hits,int current)const{
        for(size_t i=0;i<hits.size();i++){
            const SearchHit&h=hits[i];
            if(dark_mode_){
                if((int)i==current)cairo_set_source_rgba(cr,1.00,0.50,0.10,0.65);
                else                cairo_set_source_rgba(cr,0.95,0.75,0.10,0.45);
            } else {
                if((int)i==current)cairo_set_source_rgba(cr,1.0,0.55,0.0,0.55);
                else                cairo_set_source_rgba(cr,1.0,0.92,0.0,0.50);
            }
            cairo_rectangle(cr,h.doc_x-sx_,h.doc_y-sy_,h.width,h.height);
            cairo_fill(cr);
        }
    }

    void set_dark_mode(bool dark){dark_mode_=dark;}

    void draw_selection_overlay(cairo_t *cr)const{
        auto sel=get_selected_sorted();if(sel.empty())return;auto lines=group_lines(sel);
        int sx,sy,ex,ey;selection_bounds(sx,sy,ex,ey);
        if(dark_mode_)cairo_set_source_rgba(cr,0.35,0.55,1.0,0.35);
        else          cairo_set_source_rgba(cr,0.20,0.50,0.90,0.30);
        for(int li=0;li<(int)lines.size();li++){auto&lg=lines[li];int lcy=sel[lg.start]->y+sel[lg.start]->height/2;
            /* Half-open match to span_in's on_anchor / on_extent: a line is
               the anchor's (or extent's) line when the cursor y falls in
               (lcy - t, lcy + t]. The exact boundary y belongs to the line
               BELOW so we don't end up marking both adjacent lines as
               "current". */
            int line_t=(lg.max_bot-lg.min_top)/2;
            bool is_f=(lcy<=sy+line_t)&&(lcy>sy-line_t);
            bool is_l=(lcy<=ey+line_t)&&(lcy>ey-line_t);
            int hl_l=sel[lg.start]->x,hl_r=sel[lg.end-1]->x+sel[lg.end-1]->width;
            if(is_f)hl_l=clip_left(sel,lg,sx);if(is_l)hl_r=clip_right(sel,lg,ex);
            if(hl_r>hl_l){cairo_rectangle(cr,hl_l-sx_,lg.min_top-sy_,hl_r-hl_l,lg.max_bot-lg.min_top);cairo_fill(cr);}}}

    std::string get_selected_text()const{
        auto sel=get_selected_sorted();if(sel.empty())return{};auto lines=group_lines(sel);
        int sx,sy,ex,ey;selection_bounds(sx,sy,ex,ey);std::string res;int last_y=-9999;
        for(int li=0;li<(int)lines.size();li++){auto&lg=lines[li];int lcy=sel[lg.start]->y+sel[lg.start]->height/2;
            int line_t=(lg.max_bot-lg.min_top)/2;
            bool is_f=(lcy<=sy+line_t)&&(lcy>sy-line_t);
            bool is_l=(lcy<=ey+line_t)&&(lcy>ey-line_t);
            for(int j=lg.start;j<lg.end;j++){const TextSpan&sp=*sel[j];int cy=sp.y+sp.height/2;
                int bs=0,be=(int)sp.text.size();
                if(is_f&&j==lg.start&&sx>sp.x&&sx<sp.x+sp.width)bs=x_to_index(sp,sx);
                if(is_l&&j==lg.end-1&&ex>sp.x&&ex<sp.x+sp.width)be=x_to_index(sp,ex);
                if(be<=bs)continue;std::string fr=sp.text.substr(bs,be-bs);
                while(!fr.empty()&&isspace((unsigned char)fr.front()))fr.erase(fr.begin());
                while(!fr.empty()&&isspace((unsigned char)fr.back()))fr.pop_back();
                if(fr.empty())continue;if(!res.empty()&&std::abs(cy-last_y)>4)res+='\n';
                else if(!res.empty()&&res.back()!='\n')res+=' ';res+=fr;last_y=cy;}}return res;}

    /* ===== document_container interface ===== */

    litehtml::uint_ptr create_font(const litehtml::font_description &descr,
                                    const litehtml::document *,
                                    litehtml::font_metrics *fm) override {
        auto *f = new CairoFont();
        f->desc = pango_font_description_new();
        pango_font_description_set_family(f->desc, descr.family.c_str());
        pango_font_description_set_absolute_size(f->desc, (int)(descr.size * PANGO_SCALE));
        pango_font_description_set_weight(f->desc, (PangoWeight)descr.weight);
        pango_font_description_set_style(f->desc, descr.style==litehtml::font_style_italic?PANGO_STYLE_ITALIC:PANGO_STYLE_NORMAL);
        f->underline = (descr.decoration_line & litehtml::text_decoration_line_underline) != 0;
        f->strikethrough = (descr.decoration_line & litehtml::text_decoration_line_line_through) != 0;
        pango_layout_set_font_description(pl_, f->desc);
        PangoContext *ctx = pango_layout_get_context(pl_);
        PangoFontMetrics *m = pango_context_get_metrics(ctx, f->desc, nullptr);
        f->ascent = pango_font_metrics_get_ascent(m)/PANGO_SCALE;
        f->descent = pango_font_metrics_get_descent(m)/PANGO_SCALE;
        f->height = f->ascent + f->descent; f->x_height = f->height;
        if (fm) { fm->ascent=f->ascent; fm->descent=f->descent; fm->height=f->height; fm->x_height=f->x_height;
                  fm->draw_spaces=(descr.style==litehtml::font_style_italic||descr.decoration_line!=0); }
        pango_font_metrics_unref(m);
        return (litehtml::uint_ptr)f;
    }
    void delete_font(litehtml::uint_ptr h)override{auto*f=(CairoFont*)h;if(f){pango_font_description_free(f->desc);delete f;}}

    litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr h)override{
        auto*f=(CairoFont*)h;if(!f)return 0;
        pango_layout_set_font_description(pl_,f->desc);pango_layout_set_text(pl_,text,-1);
        int w,hh;pango_layout_get_pixel_size(pl_,&w,&hh);return(litehtml::pixel_t)w;}

    void draw_text(litehtml::uint_ptr hdc, const char *text, litehtml::uint_ptr hFont,
                   litehtml::web_color color, const litehtml::position &pos)override{
        auto*cr=(cairo_t*)hdc;auto*f=(CairoFont*)hFont;if(!cr||!f)return;
        spans_.push_back({(int)pos.x+sx_,(int)pos.y+sy_,(int)pos.width,(int)pos.height,std::string(text),f->desc});
        set_color(cr,color);
        PangoLayout*layout=pango_cairo_create_layout(cr);
        /* Match the measurement layout: no metric hinting → glyph widths
           stay fractional under cairo_scale (no monospace wobble). */
        cairo_font_options_t *fo = cairo_font_options_create();
        cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
        pango_cairo_context_set_font_options(pango_layout_get_context(layout), fo);
        cairo_font_options_destroy(fo);
        pango_layout_set_font_description(layout,f->desc);pango_layout_set_text(layout,text,-1);
        if(f->underline||f->strikethrough){PangoAttrList*a=pango_attr_list_new();
            if(f->underline)pango_attr_list_insert(a,pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
            if(f->strikethrough)pango_attr_list_insert(a,pango_attr_strikethrough_new(TRUE));
            pango_layout_set_attributes(layout,a);pango_attr_list_unref(a);}
        cairo_move_to(cr,pos.x,pos.y);pango_cairo_show_layout(cr,layout);g_object_unref(layout);}

    litehtml::pixel_t pt_to_px(float pt)const override{return(litehtml::pixel_t)std::round(pt*96.0f/72.0f);}
    litehtml::pixel_t get_default_font_size()const override{return 16;}
    const char*get_default_font_name()const override{return"Sans";}

    void load_image(const char*src,const char*,bool)override{if(src)get_or_load(src);}
    void get_image_size(const char*src,const char*,litehtml::size&sz)override{
        sz.width=sz.height=0;if(!src)return;if(auto*i=get_or_load(src)){sz.width=i->width;sz.height=i->height;}}

    void draw_list_marker(litehtml::uint_ptr hdc,const litehtml::list_marker&mk)override{
        auto*cr=(cairo_t*)hdc;if(!cr)return;set_color(cr,mk.color);
        double cx=mk.pos.x+mk.pos.width/2.0,cy=mk.pos.y+mk.pos.height/2.0;
        switch(mk.marker_type){
        case litehtml::list_style_type_circle:cairo_arc(cr,cx,cy,mk.pos.width/2.0,0,2*M_PI);cairo_stroke(cr);break;
        case litehtml::list_style_type_disc:cairo_arc(cr,cx,cy,mk.pos.width/2.0,0,2*M_PI);cairo_fill(cr);break;
        case litehtml::list_style_type_square:cairo_rectangle(cr,mk.pos.x,mk.pos.y,mk.pos.width,mk.pos.height);cairo_fill(cr);break;
        default:break;}}

    /* New master API: draw_background is split into individual methods */
    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                          const litehtml::web_color &color)override{
        auto*cr=(cairo_t*)hdc;if(!cr)return;set_color(cr,color);
        cairo_rectangle(cr,layer.border_box.x,layer.border_box.y,layer.border_box.width,layer.border_box.height);
        cairo_fill(cr);}

    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                     const std::string &url, const std::string &)override{
        auto*cr=(cairo_t*)hdc;if(!cr)return;
        auto*img=get_or_load(url);if(!img)return;
        cairo_save(cr);
        cairo_rectangle(cr,layer.clip_box.x,layer.clip_box.y,layer.clip_box.width,layer.clip_box.height);
        cairo_clip(cr);cairo_translate(cr,layer.origin_box.x,layer.origin_box.y);
        if(img->is_svg()){img->render_svg(cr,layer.origin_box.width,layer.origin_box.height);}
        else if(img->surface){
            double sx=(double)layer.origin_box.width/img->width,sy=(double)layer.origin_box.height/img->height;
            cairo_scale(cr,sx,sy);cairo_set_source_surface(cr,img->surface,0,0);
            cairo_pattern_set_filter(cairo_get_source(cr),CAIRO_FILTER_BILINEAR);cairo_paint(cr);}
        cairo_restore(cr);}

    void draw_linear_gradient(litehtml::uint_ptr,const litehtml::background_layer&,const litehtml::background_layer::linear_gradient&)override{}
    void draw_radial_gradient(litehtml::uint_ptr,const litehtml::background_layer&,const litehtml::background_layer::radial_gradient&)override{}
    void draw_conic_gradient(litehtml::uint_ptr,const litehtml::background_layer&,const litehtml::background_layer::conic_gradient&)override{}

    void draw_borders(litehtml::uint_ptr hdc,const litehtml::borders&b,const litehtml::position&p,bool)override{
        auto*cr=(cairo_t*)hdc;if(!cr)return;
        auto db=[&](const litehtml::border&br,double x1,double y1,double x2,double y2){
            if(br.width>0&&br.style>litehtml::border_style_hidden){set_color(cr,br.color);cairo_set_line_width(cr,br.width);
                cairo_move_to(cr,x1,y1);cairo_line_to(cr,x2,y2);cairo_stroke(cr);}};
        db(b.top,p.left(),p.top()+b.top.width/2.0,p.right(),p.top()+b.top.width/2.0);
        db(b.bottom,p.left(),p.bottom()-b.bottom.width/2.0,p.right(),p.bottom()-b.bottom.width/2.0);
        db(b.left,p.left()+b.left.width/2.0,p.top(),p.left()+b.left.width/2.0,p.bottom());
        db(b.right,p.right()-b.right.width/2.0,p.top(),p.right()-b.right.width/2.0,p.bottom());}

    void set_clip(const litehtml::position&pos,const litehtml::border_radiuses&)override{
        if(!rcr_)return;cairo_save(rcr_);cairo_rectangle(rcr_,pos.x,pos.y,pos.width,pos.height);cairo_clip(rcr_);}
    void del_clip()override{if(rcr_)cairo_restore(rcr_);}

    void set_caption(const char*)override{}
    void set_base_url(const char*)override{}
    void link(const std::shared_ptr<litehtml::document>&,const litehtml::element::ptr&)override{}
    void on_anchor_click(const char *url, const litehtml::element::ptr&)override{
        if (url && anchor_click_cb_) anchor_click_cb_(std::string(url));
    }
    void set_anchor_click_cb(std::function<void(const std::string&)> cb){anchor_click_cb_=std::move(cb);}
    void on_mouse_event(const litehtml::element::ptr&,litehtml::mouse_event)override{}
    void set_cursor(const char *name) override {
        if (cursor_cb_) cursor_cb_(name ? std::string(name) : std::string());
    }
    void set_cursor_cb(std::function<void(const std::string&)> cb){cursor_cb_=std::move(cb);}
    void import_css(litehtml::string&,const litehtml::string&,litehtml::string&)override{}
    void transform_text(litehtml::string&text,litehtml::text_transform tt)override{
        if(tt==litehtml::text_transform_uppercase)for(auto&c:text)c=(char)toupper((unsigned char)c);
        else if(tt==litehtml::text_transform_lowercase)for(auto&c:text)c=(char)tolower((unsigned char)c);}
    void get_viewport(litehtml::position&c)const override{c.x=c.y=0;c.width=vw_;c.height=vh_;}
    litehtml::element::ptr create_element(const char*,const litehtml::string_map&,const std::shared_ptr<litehtml::document>&)override{return nullptr;}
    void get_media_features(litehtml::media_features&m)const override{
        m.type=litehtml::media_type_screen;m.width=vw_;m.height=vh_;
        m.device_width=vw_;m.device_height=vh_;m.color=8;m.monochrome=0;m.color_index=256;m.resolution=96;}
    void get_language(litehtml::string&l,litehtml::string&c)const override{l="en";c="en-US";}
    litehtml::string resolve_color(const litehtml::string&)const override{return{};}

private:
    enum class CharClass { Space, Word, Punct };
    /* For UTF-8 multibyte chars, every byte (including continuation bytes
       0x80-0xBF) is >= 0x80 and classified as Word — which is what we want,
       since the typical use is letters in scripts beyond ASCII. */
    static CharClass classify(unsigned char c) {
        if (isspace(c))                            return CharClass::Space;
        if (isalnum(c) || c == '_' || c >= 0x80)   return CharClass::Word;
        return CharClass::Punct;
    }
    static void set_color(cairo_t*cr,const litehtml::web_color&c){
        cairo_set_source_rgba(cr,c.red/255.0,c.green/255.0,c.blue/255.0,c.alpha/255.0);}
    /* Lines own y-ranges as half-open intervals [cy - t, cy + t), with
       t = sp.height/2. At the exact boundary between two lines the LOWER
       line owns the cursor — there is no "between two lines" state, no
       both-partial flicker, just a clean atomic switch at the midpoint.

       `before_*` is inclusive (`cy <= sy - t`) and `after_*` is strict
       (`cy > sy + t`); flipping the boundary y from on-this-line to
       above-this-line happens at the same instant for both sides. */
    bool span_in(const TextSpan&sp,int sx,int sy,int ex,int ey)const{
        int cy = sp.y + sp.height/2;
        int t  = sp.height/2;
        bool after_anchor  = cy >  sy + t;
        bool before_anchor = cy <= sy - t;
        bool before_extent = cy <= ey - t;
        bool after_extent  = cy >  ey + t;

        /* Both endpoints land on this line. get_ordered may have produced
           sx > ex (it sorts strictly by y, so a same-line drag whose y
           wobbles even one pixel swaps the x's). Normalise here so the
           selection doesn't visibly empty whenever the cursor wobbles
           vertically within the anchor's line. */
        if (!after_anchor && !before_anchor && !before_extent && !after_extent) {
            int xlo = sx < ex ? sx : ex;
            int xhi = sx < ex ? ex : sx;
            return sp.x + sp.width > xlo && sp.x < xhi;
        }

        bool af = after_anchor  ? true : before_anchor ? false : (sp.x + sp.width > sx);
        bool bf = before_extent ? true : after_extent  ? false : (sp.x < ex);
        return af && bf;
    }

    int vw_=800,vh_=600,sy_=0,sx_=0;
    std::string bd_;
    cairo_surface_t*ss_;cairo_t*sc_;PangoLayout*pl_;
    cairo_t*rcr_=nullptr;
    std::vector<TextSpan>spans_;Selection sel_;
    std::map<std::string,CachedImage*>imgs_;
    std::function<void(const std::string&)> anchor_click_cb_;
    std::function<void(const std::string&)> cursor_cb_;
    bool dark_mode_=false;
};

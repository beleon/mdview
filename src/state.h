#pragma once

/* Persisted preferences. Flat key=value file at $XDG_CONFIG_HOME/mdview/state
   (or $HOME/.config/mdview/state).

   Global preferences (zoom, window size, theme, search flags) plus a small
   bounded LRU map of per-file view state — scroll position and zoom —
   keyed by a 64-bit hash of the file's canonical path. The hash means an
   observer glancing at the state file sees opaque tokens, not a list of
   paths you've opened.

   Global `zoom` doubles as the default for any file that isn't in the
   LRU yet — so the zoom level you last closed becomes what new files
   inherit. */

#include <sys/stat.h>
#include <sys/types.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace mdv {

struct FileView { double scroll_x = 0, scroll_y = 0; double zoom = 1.0; };

struct State {
    double zoom            = 1.0;
    int    window_width    = 900;
    int    window_height   = 700;
    bool   dark_mode       = false;
    bool   case_sensitive  = false;
    bool   whole_word      = false;
    bool   regex_mode      = false;
    /* Per-file view-state LRU. Vector (not map) so we can preserve recency
       order — most recently accessed at the back, oldest at the front,
       capped at FILES_LRU_MAX entries. Key is FNV1a-64(canonical_path). */
    std::vector<std::pair<uint64_t, FileView>> files;
};

static const size_t FILES_LRU_MAX = 20;

/* 64-bit FNV1a. We're hashing paths for identity ("is this the same file?"),
   not for crypto, so this trivial mixer is plenty. Stable across runs and
   machines for the same byte sequence — which is what matters. */
static inline uint64_t fnv1a64(const std::string &s) {
    uint64_t h = 14695981039346656037ull;
    for (char c : s) { h ^= (uint8_t)c; h *= 1099511628211ull; }
    return h;
}

static inline std::string hash_hex(uint64_t h) {
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

static inline bool hex_hash(const std::string &hex, uint64_t &out) {
    if (hex.size() != 16) return false;
    out = 0;
    for (char c : hex) {
        out <<= 4;
        if      (c >= '0' && c <= '9') out |= (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') out |= (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') out |= (uint64_t)(c - 'A' + 10);
        else return false;
    }
    return true;
}

/* LRU touch: if `h` already has an entry, drop it; push the new value at
   the back so it's marked most-recently-used; truncate front-side if we
   exceed the cap. */
static inline void files_touch(State &s, uint64_t h, FileView v) {
    for (auto it = s.files.begin(); it != s.files.end(); ) {
        if (it->first == h) it = s.files.erase(it);
        else ++it;
    }
    s.files.push_back({h, v});
    while (s.files.size() > FILES_LRU_MAX) s.files.erase(s.files.begin());
}

/* Returns "$XDG_CONFIG_HOME/mdview/state", falling back to
   "$HOME/.config/mdview/state". Empty string on failure. */
static inline std::string state_path() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else if (home && *home) { base = home; base += "/.config"; }
    else return "";
    return base + "/mdview/state";
}

static inline void ensure_parent_dir(const std::string &path) {
    /* mkdir -p on the parent. Walk forward, creating each component. */
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return;
    std::string dir = path.substr(0, slash);
    size_t pos = (dir[0] == '/') ? 1 : 0;
    while (pos <= dir.size()) {
        size_t next = dir.find('/', pos);
        std::string sub = (next == std::string::npos) ? dir : dir.substr(0, next);
        if (!sub.empty()) mkdir(sub.c_str(), 0755);  /* ignore EEXIST */
        if (next == std::string::npos) break;
        pos = next + 1;
    }
}

static inline State load_state() {
    State s;
    std::string p = state_path();
    if (p.empty()) return s;
    std::ifstream f(p);
    if (!f.is_open()) return s;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        /* `view:<16-hex>=<scroll_x>,<scroll_y>,<zoom>` — hash key is fixed-width
           so we can split unambiguously after the prefix. We still accept
           the legacy `scroll:<hex>=<x>,<y>` lines from before per-file zoom
           was added; zoom defaults to 1.0 there. */
        bool is_view   = (line.compare(0, 5, "view:")   == 0);
        bool is_scroll = (line.compare(0, 7, "scroll:") == 0);
        if (is_view || is_scroll) {
            size_t prefix_len = is_view ? 5 : 7;
            size_t eq = line.find('=', prefix_len);
            if (eq == std::string::npos) continue;
            std::string hex = line.substr(prefix_len, eq - prefix_len);
            std::string val = line.substr(eq + 1);
            uint64_t h;
            if (!hex_hash(hex, h)) continue;
            /* Split val on commas. */
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t i = 0; i <= val.size(); i++) {
                if (i == val.size() || val[i] == ',') {
                    parts.push_back(val.substr(start, i - start));
                    start = i + 1;
                }
            }
            if (parts.size() < 2) continue;
            FileView fv;
            fv.scroll_x = atof(parts[0].c_str());
            fv.scroll_y = atof(parts[1].c_str());
            if (parts.size() >= 3) fv.zoom = atof(parts[2].c_str());
            s.files.push_back({h, fv});
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if      (key == "zoom")           s.zoom = atof(val.c_str());
        else if (key == "window_width")   s.window_width = atoi(val.c_str());
        else if (key == "window_height")  s.window_height = atoi(val.c_str());
        else if (key == "dark_mode")      s.dark_mode      = (val == "1" || val == "true");
        else if (key == "case_sensitive") s.case_sensitive = (val == "1" || val == "true");
        else if (key == "whole_word")     s.whole_word     = (val == "1" || val == "true");
        else if (key == "regex_mode")     s.regex_mode     = (val == "1" || val == "true");
    }
    /* Clamp to LRU cap on load too, defending against a hand-edited oversize file. */
    while (s.files.size() > FILES_LRU_MAX) s.files.erase(s.files.begin());
    return s;
}

static inline void save_state(const State &s) {
    std::string p = state_path();
    if (p.empty()) return;
    ensure_parent_dir(p);
    /* Atomic write: tmp file + rename. Survives a crash mid-write. */
    std::string tmp = p + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) return;
        f << "# mdview persisted state — auto-generated\n";
        f << "zoom="           << s.zoom           << "\n";
        f << "window_width="   << s.window_width   << "\n";
        f << "window_height="  << s.window_height  << "\n";
        f << "dark_mode="      << (s.dark_mode ? 1 : 0)      << "\n";
        f << "case_sensitive=" << (s.case_sensitive ? 1 : 0) << "\n";
        f << "whole_word="     << (s.whole_word ? 1 : 0)     << "\n";
        f << "regex_mode="     << (s.regex_mode ? 1 : 0)     << "\n";
        for (const auto &kv : s.files) {
            f << "view:" << hash_hex(kv.first) << "="
              << kv.second.scroll_x << ","
              << kv.second.scroll_y << ","
              << kv.second.zoom << "\n";
        }
    }
    rename(tmp.c_str(), p.c_str());
}

}  /* namespace mdv */

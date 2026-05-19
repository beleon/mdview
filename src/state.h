#pragma once

/* Persisted preferences. Flat key=value file at $XDG_CONFIG_HOME/mdview/state
   (or $HOME/.config/mdview/state).

   Global preferences (zoom, window size, theme, search flags) plus a small
   bounded LRU map of per-file scroll positions, keyed by a 64-bit hash of
   the file's canonical path. The hash means an observer glancing at the
   state file sees opaque tokens, not a list of paths you've opened. */

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

struct ScrollPos { double x = 0, y = 0; };

struct State {
    double zoom            = 1.0;
    int    window_width    = 900;
    int    window_height   = 700;
    bool   dark_mode       = false;
    bool   case_sensitive  = false;
    bool   whole_word      = false;
    bool   regex_mode      = false;
    /* Scroll-position LRU. Vector (not map) so we can preserve recency
       order — most recently accessed at the back, oldest at the front,
       capped at SCROLL_LRU_MAX entries. Key is FNV1a-64(canonical_path). */
    std::vector<std::pair<uint64_t, ScrollPos>> scroll;
};

static const size_t SCROLL_LRU_MAX = 20;

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
static inline void scroll_touch(State &s, uint64_t h, ScrollPos p) {
    for (auto it = s.scroll.begin(); it != s.scroll.end(); ) {
        if (it->first == h) it = s.scroll.erase(it);
        else ++it;
    }
    s.scroll.push_back({h, p});
    while (s.scroll.size() > SCROLL_LRU_MAX) s.scroll.erase(s.scroll.begin());
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
        /* `scroll:<16-hex>=<x>,<y>` — hash key is fixed-width, so we can
           split unambiguously after the "scroll:" prefix. */
        if (line.compare(0, 7, "scroll:") == 0) {
            size_t eq = line.find('=', 7);
            if (eq == std::string::npos) continue;
            std::string hex = line.substr(7, eq - 7);
            std::string val = line.substr(eq + 1);
            uint64_t h;
            if (!hex_hash(hex, h)) continue;
            size_t comma = val.find(',');
            if (comma == std::string::npos) continue;
            ScrollPos sp;
            sp.x = atof(val.substr(0, comma).c_str());
            sp.y = atof(val.substr(comma + 1).c_str());
            s.scroll.push_back({h, sp});
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
    while (s.scroll.size() > SCROLL_LRU_MAX) s.scroll.erase(s.scroll.begin());
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
        for (const auto &kv : s.scroll) {
            f << "scroll:" << hash_hex(kv.first) << "="
              << kv.second.x << "," << kv.second.y << "\n";
        }
    }
    rename(tmp.c_str(), p.c_str());
}

}  /* namespace mdv */

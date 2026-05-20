#pragma once

/* User-editable config file with defaults for newly-opened files.
   Lives at $XDG_CONFIG_HOME/mdview/config (or $HOME/.config/mdview/config),
   distinct from the auto-managed `state` file in the same directory.

   What goes here vs. state:
     - config (this file): user's manually-edited preferences. Read-only
       from mdview's perspective. We never write to it. Applies to files
       that aren't in the per-file LRU yet, i.e. the first time you open
       something.
     - state: machine-managed remembered preferences. Per-file scroll/zoom/
       font-size LRU, last window size, dark mode, search flags. We do
       write here on every close.

   Once a file is in the LRU, its remembered settings always win over
   what's in config.

   Format is `key = value` (whitespace tolerated), one per line, `#`
   comments. Recognised keys:
     font_size  — base body font-size in px (default 16)
     zoom       — pinch-zoom level, 1.0 = 100% (default 1.0)
*/

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace mdv {

struct Config {
    double zoom      = 1.0;
    int    font_size = 16;
};

static inline std::string config_path() {
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    std::string base;
    if (xdg && *xdg)        base = xdg;
    else if (home && *home) { base = home; base += "/.config"; }
    else                    return "";
    return base + "/mdview/config";
}

static inline std::string cfg_trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) b--;
    return s.substr(a, b - a);
}

static inline Config load_config() {
    Config c;
    std::string p = config_path();
    if (p.empty()) return c;
    std::ifstream f(p);
    if (!f.is_open()) return c;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = cfg_trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = cfg_trim(t.substr(0, eq));
        std::string val = cfg_trim(t.substr(eq + 1));
        if      (key == "font_size") c.font_size = atoi(val.c_str());
        else if (key == "zoom")      c.zoom      = atof(val.c_str());
    }
    return c;
}

}  /* namespace mdv */

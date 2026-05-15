#pragma once

/* Persisted preferences. Flat key=value file at $XDG_CONFIG_HOME/mdview/state
   (or $HOME/.config/mdview/state). All fields are global, not per-file. */

#include <sys/stat.h>
#include <sys/types.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace mdv {

struct State {
    double zoom            = 1.0;
    int    window_width    = 900;
    int    window_height   = 700;
    bool   dark_mode       = false;
    bool   case_sensitive  = false;
    bool   whole_word      = false;
    bool   regex_mode      = false;
};

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
    /* Walk forward through dir's components. */
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
    }
    rename(tmp.c_str(), p.c_str());
}

}  /* namespace mdv */

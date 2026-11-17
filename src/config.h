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
#include <sstream>
#include <string>

namespace mdv {

struct Chrome {
  double bg[3];
  double text[3];
  double dim[3];
  double sep[3];
  double accent[3];
  double accent_bg[4]; /* with alpha */
};
static Chrome CHROME_LIGHT = {{0.97, 0.97, 0.97}, {0.10, 0.10, 0.10},
                              {0.45, 0.45, 0.45}, {0.60, 0.60, 0.60},
                              {0.20, 0.50, 0.90}, {0.20, 0.50, 0.90, 0.18}};
static Chrome CHROME_DARK = {{0.16, 0.16, 0.16}, {0.85, 0.85, 0.85},
                             {0.60, 0.60, 0.60}, {0.30, 0.30, 0.30},
                             {0.50, 0.70, 1.00}, {0.40, 0.60, 1.00, 0.20}};

struct Theme {
  std::string body_bg;
  std::string body_color;
  std::string h_border_strong;
  std::string h_border_light;
  std::string code_bg;
  std::string code_border;
  std::string bq_border;
  std::string bq_color;
  std::string link;
  std::string th_bg;
  std::string border;
  std::string hr_border;
};

static Theme THEME_LIGHT = {"#ffffff", "#222222", "#cccccc", "#eeeeee",
                            "#f4f4f4", "#dddddd", "#dddddd", "#555555",
                            "#0066cc", "#f0f0f0", "#dddddd", "#dddddd"};

static Theme THEME_DARK = {"#1e1e1e", "#d4d4d4", "#444444", "#333333",
                           "#2a2a2a", "#3a3a3a", "#3a3a3a", "#999999",
                           "#6db5ff", "#2a2a2a", "#3a3a3a", "#3a3a3a"};

struct Config {
  double zoom = 1.0;
  int font_size = 16;
};

static inline std::string config_path() {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  std::string base;
  if (xdg && *xdg)
    base = xdg;
  else if (home && *home) {
    base = home;
    base += "/.config";
  } else
    return "";
  return base + "/mdview/config";
}

static inline std::string theme_path(std::string name) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  std::string base;
  if (xdg && *xdg)
    base = xdg;
  else if (home && *home) {
    base = home;
    base += "/.config";
  } else
    return "";
  return base + "/mdview/" + name;
}

static inline std::string cfg_trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t'))
    a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
    b--;
  return s.substr(a, b - a);
}

static inline void parse_color(const std::string &value, double *out,
                               size_t count) {
  std::istringstream ss(value);

  for (size_t i = 0; i < count; i++) {
    if (!(ss >> out[i]))
      return;

    if (i + 1 < count) {
      char comma;
      if (!(ss >> comma) || comma != ',')
        return;
    }
  }
}

static inline void load_theme(Chrome *chrome, Theme *theme, std::string name) {
  if (!theme || !chrome)
    return;
  std::string p = theme_path(name);
  if (p.empty())
    return;
  std::ifstream f(p);
  if (!f.is_open())
    return;
  std::string line;
  while (std::getline(f, line)) {
    std::string t = cfg_trim(line);
    if (t.empty() || t[0] == '#')
      continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = cfg_trim(t.substr(0, eq));
    std::string val = cfg_trim(t.substr(eq + 1));
    if (key == "body_bg")
      theme->body_bg = val;
    else if (key == "body_color")
      theme->body_color = val;
    else if (key == "h_border_strong")
      theme->h_border_strong = val;
    else if (key == "h_border_light")
      theme->h_border_light = val;
    else if (key == "code_bg")
      theme->code_bg = val;
    else if (key == "code_border")
      theme->code_border = val;
    else if (key == "bq_border")
      theme->bq_border = val;
    else if (key == "bq_color")
      theme->bq_color = val;
    else if (key == "link")
      theme->link = val;
    else if (key == "th_bg")
      theme->th_bg = val;
    else if (key == "border")
      theme->border = val;
    else if (key == "hr_border")
      theme->hr_border = val;
    else if (key == "chrome_bg")
      parse_color(val, chrome->bg, 3);
    else if (key == "chrome_text")
      parse_color(val, chrome->text, 3);
    else if (key == "chrome_dim")
      parse_color(val, chrome->dim, 3);
    else if (key == "chrome_sep")
      parse_color(val, chrome->sep, 3);
    else if (key == "chrome_accent")
      parse_color(val, chrome->accent, 3);
    else if (key == "chrome_accent_bg")
      parse_color(val, chrome->accent_bg, 4);
  }
}

static inline Config load_config() {
  load_theme(&CHROME_DARK, &THEME_DARK, "dark_theme");
  load_theme(&CHROME_LIGHT, &THEME_LIGHT, "light_theme");
  Config c;
  std::string p = config_path();
  if (p.empty())
    return c;
  std::ifstream f(p);
  if (!f.is_open())
    return c;
  std::string line;
  while (std::getline(f, line)) {
    std::string t = cfg_trim(line);
    if (t.empty() || t[0] == '#')
      continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = cfg_trim(t.substr(0, eq));
    std::string val = cfg_trim(t.substr(eq + 1));
    if (key == "font_size")
      c.font_size = atoi(val.c_str());
    else if (key == "zoom")
      c.zoom = atof(val.c_str());
  }
  return c;
}

} /* namespace mdv */

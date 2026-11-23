#pragma once

/* `mdview --install` / `--uninstall` — local XDG installation.

   Lays out the standard freedesktop locations under $XDG_DATA_HOME (or
   ~/.local/share if unset) plus $XDG_BIN_HOME (or ~/.local/bin), so a user
   who extracted the tarball anywhere can run `./mdview --install` and have
   the binary, icons, and .desktop entry land in the right places without
   needing root or a package manager.

   What gets installed:
     - the running binary       → $XDG_BIN_HOME/mdview
     - one PNG per embedded size → $XDG_DATA_HOME/icons/hicolor/<N>x<N>/apps/mdview.png
     - a desktop entry          → $XDG_DATA_HOME/applications/mdview.desktop

   The desktop entry's Exec= points at the freshly-installed binary path
   so it works even if the source tarball is later deleted. */

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "icons_data.h"

namespace mdv {

static inline std::string xdg_path(const char *var, const char *fallback_under_home) {
    const char *e = getenv(var);
    if (e && *e) return e;
    const char *h = getenv("HOME");
    if (!h || !*h) return "";
    return std::string(h) + fallback_under_home;
}

static inline bool mkdir_p(const std::string &path) {
    if (path.empty()) return false;
    std::string s = path;
    for (size_t i = 1; i < s.size(); i++) {
        if (s[i] != '/') continue;
        s[i] = '\0';
        if (mkdir(s.c_str(), 0755) < 0 && errno != EEXIST) { s[i] = '/'; return false; }
        s[i] = '/';
    }
    if (mkdir(s.c_str(), 0755) < 0 && errno != EEXIST) return false;
    return true;
}

static inline bool write_file(const std::string &path, const void *data, size_t len, mode_t mode = 0644) {
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !mkdir_p(path.substr(0, slash))) return false;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    const char *p = (const char*)data;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) { if (errno == EINTR) continue; close(fd); return false; }
        p += n; left -= (size_t)n;
    }
    return close(fd) == 0;
}

/* Read the currently-running binary into a buffer and write it out at dst.
   Copying through user-space is enough at our size and avoids needing
   sendfile/copy_file_range portability dances. */
static inline bool copy_self(const std::string &dst) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0) return false;
    self[n] = '\0';

    /* No-op if user is reinstalling from the install location. */
    char dst_real[PATH_MAX];
    if (realpath(dst.c_str(), dst_real) && strcmp(dst_real, self) == 0) return true;

    size_t slash = dst.find_last_of('/');
    if (slash != std::string::npos && !mkdir_p(dst.substr(0, slash))) return false;

    int sfd = open(self, O_RDONLY);
    if (sfd < 0) return false;
    int dfd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dfd < 0) { close(sfd); return false; }

    char buf[32768];
    for (;;) {
        ssize_t r = read(sfd, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0) { if (errno == EINTR) continue; close(sfd); close(dfd); return false; }
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(dfd, buf + off, r - off);
            if (w < 0) { if (errno == EINTR) continue; close(sfd); close(dfd); return false; }
            off += w;
        }
    }
    close(sfd);
    return close(dfd) == 0;
}

static inline int do_install() {
    std::string data = xdg_path("XDG_DATA_HOME", "/.local/share");
    std::string bin  = xdg_path("XDG_BIN_HOME",  "/.local/bin");
    if (data.empty() || bin.empty()) {
        fprintf(stderr, "mdview --install: $HOME not set, can't determine XDG paths\n");
        return 1;
    }

    std::string bin_path = bin + "/mdview";
    if (!copy_self(bin_path)) {
        fprintf(stderr, "mdview --install: failed to copy binary to %s: %s\n",
                bin_path.c_str(), strerror(errno));
        return 1;
    }
    printf("installed binary  → %s\n", bin_path.c_str());

    int n_ok = 0;
    for (int i = 0; i < MDV_ICONS_N; i++) {
        int sz = MDV_ICONS[i].size;
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/icons/hicolor/%dx%d/apps/mdview.png",
                 data.c_str(), sz, sz);
        if (write_file(p, MDV_ICONS[i].data, MDV_ICONS[i].len)) n_ok++;
        else fprintf(stderr, "  warning: failed to write %s: %s\n", p, strerror(errno));
    }
    printf("installed icons   → %s/icons/hicolor/  (%d sizes)\n", data.c_str(), n_ok);

    std::string desktop_path = data + "/applications/mdview.desktop";
    std::string desktop =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=mdview\n"
        "GenericName=Markdown Viewer\n"
        "Comment=Lightweight Wayland-native Markdown viewer\n"
        /* -f keeps mdview attached to its launcher (no fork+detach). When the
           launcher is a GUI shell (gnome-shell, KDE, etc.) it tracks the
           process via systemd app-scope cgroups; if we detach, the launcher
           sees the parent exit immediately, the startup indicator hangs, and
           cgroup tracking breaks. Terminal invocations of `mdview foo.md`
           still detach by default — only this desktop-launch path needs the
           flag. */
        "Exec=" + bin_path + " -f %F\n"
        "Icon=mdview\n"
        "MimeType=text/markdown;\n"
        "Categories=Office;Viewer;TextTools;\n"
        "StartupNotify=true\n"
        "Terminal=false\n"
        /* mdview needs a file to show; launched bare it can only print its
           usage, which a launcher swallows. NoDisplay keeps it out of menus
           while leaving the text/markdown association, so it is still offered
           under "Open With" and can be the default .md handler. */
        "NoDisplay=true\n";
    if (!write_file(desktop_path, desktop.data(), desktop.size())) {
        fprintf(stderr, "mdview --install: failed to write %s: %s\n",
                desktop_path.c_str(), strerror(errno));
        return 1;
    }
    printf("installed entry   → %s\n", desktop_path.c_str());

    /* Friendly nudge if ~/.local/bin isn't on PATH. */
    const char *pathenv = getenv("PATH");
    if (!pathenv || !strstr(pathenv, bin.c_str())) {
        printf("\nNote: %s is not on $PATH. Either add it:\n", bin.c_str());
        printf("    export PATH=\"%s:$PATH\"\n", bin.c_str());
        printf("or invoke mdview by its full path.\n");
    }
    return 0;
}

static inline int do_uninstall() {
    std::string data = xdg_path("XDG_DATA_HOME", "/.local/share");
    std::string bin  = xdg_path("XDG_BIN_HOME",  "/.local/bin");
    if (data.empty() || bin.empty()) return 1;

    auto try_unlink = [](const std::string &p) {
        if (unlink(p.c_str()) == 0) printf("removed: %s\n", p.c_str());
    };
    try_unlink(bin + "/mdview");
    for (int i = 0; i < MDV_ICONS_N; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/icons/hicolor/%dx%d/apps/mdview.png",
                 data.c_str(), MDV_ICONS[i].size, MDV_ICONS[i].size);
        try_unlink(p);
    }
    try_unlink(data + "/applications/mdview.desktop");
    return 0;
}

}  /* namespace mdv */

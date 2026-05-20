#pragma once

/* inotify-based file watcher with debounce. Watches the *directory*
   containing the file (so atomic rename-based saves are caught; many editors
   write to a temp file and rename over the target), filters events by
   basename, and arms a one-shot 100ms timerfd. When the timer fires, calls
   the user's reload callback. */

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>
#include <string>

extern "C" {
#include "window.h"
}

namespace mdv {

class LiveReloader {
public:
    typedef void (*ReloadFn)(void *userdata);

    LiveReloader() = default;
    LiveReloader(const LiveReloader&) = delete;
    LiveReloader& operator=(const LiveReloader&) = delete;
    ~LiveReloader() { stop(); }

    /* Start watching `file_path`. Returns true on success. */
    bool start(mdv_window *win, const std::string &file_path,
               ReloadFn cb, void *userdata) {
        win_      = win;
        callback_ = cb;
        userdata_ = userdata;

        /* Split into dir + basename. */
        size_t slash = file_path.find_last_of('/');
        std::string dir;
        if (slash == std::string::npos) { dir = "."; basename_ = file_path; }
        else                            { dir = file_path.substr(0, slash); basename_ = file_path.substr(slash + 1); }
        if (dir.empty()) dir = ".";

        inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0) return false;
        int wd = inotify_add_watch(inotify_fd_, dir.c_str(),
                                   IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        if (wd < 0) { close(inotify_fd_); inotify_fd_ = -1; return false; }

        timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timer_fd_ < 0) { close(inotify_fd_); inotify_fd_ = -1; return false; }

        if (mdv_window_watch_fd(win_, inotify_fd_, on_inotify_static, this) != 0 ||
            mdv_window_watch_fd(win_, timer_fd_, on_timer_static, this) != 0) {
            stop();
            return false;
        }
        return true;
    }

    void stop() {
        if (win_) {
            if (inotify_fd_ >= 0) mdv_window_unwatch_fd(win_, inotify_fd_);
            if (timer_fd_ >= 0)   mdv_window_unwatch_fd(win_, timer_fd_);
        }
        if (inotify_fd_ >= 0) { close(inotify_fd_); inotify_fd_ = -1; }
        if (timer_fd_   >= 0) { close(timer_fd_);   timer_fd_ = -1;   }
        win_ = nullptr;
    }

private:
    static const int DEBOUNCE_MS = 100;

    static void on_inotify_static(mdv_window *w, int fd, void *ud) {
        (void)w; ((LiveReloader*)ud)->on_inotify(fd);
    }
    static void on_timer_static(mdv_window *w, int fd, void *ud) {
        (void)w; ((LiveReloader*)ud)->on_timer(fd);
    }

    void on_inotify(int fd) {
        /* Drain all available events; rearm timer if any match our basename. */
        char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
        bool match = false;
        for (;;) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            for (char *p = buf; p < buf + n; ) {
                struct inotify_event *ev = (struct inotify_event*)p;
                if (ev->len > 0 && basename_ == ev->name) match = true;
                p += sizeof(struct inotify_event) + ev->len;
            }
        }
        if (!match) return;
        /* Restart 100ms one-shot timer (resets if already armed → debounce). */
        struct itimerspec its = {0};
        its.it_value.tv_sec  = DEBOUNCE_MS / 1000;
        its.it_value.tv_nsec = (DEBOUNCE_MS % 1000) * 1000000L;
        timerfd_settime(timer_fd_, 0, &its, NULL);
    }

    void on_timer(int fd) {
        uint64_t x; ssize_t r = read(fd, &x, sizeof(x)); (void)r;
        if (callback_) callback_(userdata_);
    }

    mdv_window *win_      = nullptr;
    int         inotify_fd_ = -1;
    int         timer_fd_   = -1;
    std::string basename_;
    ReloadFn    callback_ = nullptr;
    void       *userdata_ = nullptr;
};

}  /* namespace mdv */

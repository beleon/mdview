#pragma once

/* Async syntax highlighting via subprocess shell-out.

   At startup we look for one of `bat`, `pygmentize`, `highlight`, or
   `source-highlight` in $PATH. After the window's first frame is acked, we
   extract <pre><code> blocks from the rendered HTML, fork+exec the
   highlighter on each (concurrency capped at MAX_INFLIGHT), pipe the source
   in, read ANSI-coloured output back, and convert SGR escapes to inline
   <span style="color:#rrggbb"> tags. The app rebuilds the litehtml document
   with the substituted spans — once after the first MAX_INFLIGHT jobs
   finish, then once when everything is done. */

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "window.h"
}

namespace mdv {

enum class HLTool { None, Bat, Pygmentize, Highlight, SourceHighlight };

struct CodeBlock {
    size_t      content_start = 0;  /* byte offset in source HTML */
    size_t      content_end   = 0;
    std::string lang;                /* empty if no language tag */
    std::string raw_content;         /* HTML-decoded source */
};

/* ------------------------------------------------------------------------ */
/* HTML helpers                                                             */
/* ------------------------------------------------------------------------ */

static inline std::string html_decode(const std::string &s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '&') {
            if      (s.compare(i, 5, "&amp;")  == 0) { r += '&';  i += 5; }
            else if (s.compare(i, 4, "&lt;")   == 0) { r += '<';  i += 4; }
            else if (s.compare(i, 4, "&gt;")   == 0) { r += '>';  i += 4; }
            else if (s.compare(i, 6, "&quot;") == 0) { r += '"';  i += 6; }
            else if (s.compare(i, 6, "&apos;") == 0) { r += '\''; i += 6; }
            else if (s.compare(i, 5, "&#39;")  == 0) { r += '\''; i += 5; }
            else                                      { r += s[i++]; }
        } else {
            r += s[i++];
        }
    }
    return r;
}

static inline std::vector<CodeBlock> extract_code_blocks(const std::string &html) {
    std::vector<CodeBlock> blocks;
    size_t pos = 0;
    static const char OPEN[]    = "<pre><code";
    static const char OPEN_PRE[] = "<pre>";
    static const char CLOSE[]   = "</code></pre>";
    while (true) {
        size_t pre = html.find(OPEN, pos);
        if (pre == std::string::npos) break;
        /* The next '>' after `pre` closes the <pre> tag, not the <code> we
           want. Skip past "<pre>" first, then find the '>' that closes
           the <code ...> opening tag. */
        size_t code_tag_start = pre + sizeof(OPEN_PRE) - 1;
        size_t open_end = html.find('>', code_tag_start);
        if (open_end == std::string::npos) break;
        size_t content_start = open_end + 1;
        size_t close = html.find(CLOSE, content_start);
        if (close == std::string::npos) break;

        CodeBlock b;
        b.content_start = content_start;
        b.content_end   = close;

        /* The <code ...> opening tag — language attribute lives here. */
        std::string opening = html.substr(code_tag_start, content_start - code_tag_start);
        size_t lp = opening.find("language-");
        if (lp != std::string::npos) {
            size_t le = lp + 9;
            while (le < opening.size()) {
                char c = opening[le];
                if (c == '"' || c == '\'' || c == ' ' || c == '>') break;
                le++;
            }
            b.lang = opening.substr(lp + 9, le - lp - 9);
        }
        b.raw_content = html_decode(html.substr(content_start, close - content_start));

        /* The lang attribute is passed verbatim as argv to the highlighter
           subprocess. Reject anything that could be interpreted as a flag or
           contains non-language characters. Real lang names like "c++",
           "c#", "f#", "objective-c" are inside the allowed set. */
        if (!b.lang.empty()) {
            bool ok = b.lang[0] != '-';
            for (char c : b.lang) {
                if (!(isalnum((unsigned char)c) || c == '+' || c == '-'
                   || c == '_' || c == '#' || c == '.')) { ok = false; break; }
            }
            if (!ok) b.lang.clear();
        }

        blocks.push_back(std::move(b));
        pos = close + sizeof(CLOSE) - 1;
    }
    return blocks;
}

/* ------------------------------------------------------------------------ */
/* ANSI (SGR-only) → HTML span conversion                                    */
/* ------------------------------------------------------------------------ */

static inline void color256_to_rgb(int idx, int &r, int &g, int &b) {
    if (idx < 16) {
        /* Friendly-on-white approximation of the 16 ANSI colors.
           Real terminals vary; this is "good enough" for code highlighting. */
        static const int t[16][3] = {
            {  0,  0,  0}, {170,  0,  0}, {  0,170,  0}, {170, 85,  0},
            {  0,  0,170}, {170,  0,170}, {  0,170,170}, {170,170,170},
            { 85, 85, 85}, {220, 40, 40}, { 40,180, 40}, {200,150,  0},
            { 40, 40,200}, {200, 40,200}, { 40,180,180}, {220,220,220}
        };
        r = t[idx][0]; g = t[idx][1]; b = t[idx][2];
    } else if (idx < 232) {
        int v = idx - 16;
        static const int s[6] = {0, 95, 135, 175, 215, 255};
        r = s[v / 36]; g = s[(v / 6) % 6]; b = s[v % 6];
    } else {
        int gray = 8 + (idx - 232) * 10;
        if (gray > 238) gray = 238;
        r = g = b = gray;
    }
}

static inline std::string ansi_to_html_spans(const std::string &ansi) {
    std::string html; html.reserve(ansi.size() * 2);

    struct State {
        int  r = -1, g = -1, b = -1;  /* -1 = default */
        bool bold = false, italic = false, underline = false;
        bool is_default() const { return r < 0 && !bold && !italic && !underline; }
        bool eq(const State &o) const {
            return r == o.r && g == o.g && b == o.b &&
                   bold == o.bold && italic == o.italic && underline == o.underline;
        }
    } s, prev_s;
    bool span_open = false;
    std::string pending;

    auto open_span = [&]() {
        char buf[40];
        html += "<span style=\"";
        if (s.r >= 0) {
            snprintf(buf, sizeof(buf), "color:#%02x%02x%02x;", s.r, s.g, s.b);
            html += buf;
        }
        if (s.bold)      html += "font-weight:bold;";
        if (s.italic)    html += "font-style:italic;";
        if (s.underline) html += "text-decoration:underline;";
        html += "\">";
        span_open = true;
    };
    auto close_span = [&]() {
        if (span_open) { html += "</span>"; span_open = false; }
    };
    auto flush = [&]() {
        if (pending.empty()) return;
        if (!s.eq(prev_s)) {
            close_span();
            if (!s.is_default()) open_span();
            prev_s = s;
        }
        for (char c : pending) {
            if      (c == '&') html += "&amp;";
            else if (c == '<') html += "&lt;";
            else if (c == '>') html += "&gt;";
            else               html += c;
        }
        pending.clear();
    };

    size_t i = 0, n = ansi.size();
    while (i < n) {
        unsigned char c = (unsigned char)ansi[i];
        if (c != 0x1B) { pending += (char)c; i++; continue; }
        /* Escape sequence — flush text buffered with prior state. */
        flush();
        i++;
        if (i >= n) break;
        char next = ansi[i++];
        if (next == '[') {
            /* CSI: parameter bytes (0x30-0x3F) then final byte (0x40-0x7E). */
            std::string params;
            while (i < n && (unsigned char)ansi[i] < 0x40) params += ansi[i++];
            if (i >= n) break;
            char cmd = ansi[i++];
            if (cmd != 'm') continue;
            /* Parse the numeric parameters. */
            std::vector<int> nums;
            std::string num;
            for (char p : params) {
                if (p == ';' || p == ':') {
                    nums.push_back(num.empty() ? 0 : atoi(num.c_str()));
                    num.clear();
                } else if (isdigit((unsigned char)p)) {
                    num += p;
                }
            }
            if (!num.empty()) nums.push_back(atoi(num.c_str()));
            if (nums.empty()) nums.push_back(0);

            for (size_t j = 0; j < nums.size(); j++) {
                int code = nums[j];
                if      (code == 0)  s = State{};
                else if (code == 1)  s.bold = true;
                else if (code == 3)  s.italic = true;
                else if (code == 4)  s.underline = true;
                else if (code == 22) s.bold = false;
                else if (code == 23) s.italic = false;
                else if (code == 24) s.underline = false;
                else if (code == 39) { s.r = s.g = s.b = -1; }
                else if (code == 38) {
                    /* 38;2;R;G;B = truecolor; 38;5;N = 256-color */
                    if (j + 1 < nums.size() && nums[j+1] == 2 && j + 4 < nums.size()) {
                        s.r = nums[j+2]; s.g = nums[j+3]; s.b = nums[j+4];
                        j += 4;
                    } else if (j + 2 < nums.size() && nums[j+1] == 5) {
                        color256_to_rgb(nums[j+2], s.r, s.g, s.b);
                        j += 2;
                    }
                }
                else if (code >= 30 && code <= 37) color256_to_rgb(code - 30, s.r, s.g, s.b);
                else if (code >= 90 && code <= 97) color256_to_rgb(code - 90 + 8, s.r, s.g, s.b);
                /* Background colors (40-47, 100-107) and other codes — ignored. */
            }
        } else if (next == ']') {
            /* OSC: read until BEL or ST (ESC \). */
            while (i < n) {
                if (ansi[i] == '\x07') { i++; break; }
                if (ansi[i] == 0x1B && i + 1 < n && ansi[i+1] == '\\') { i += 2; break; }
                i++;
            }
        }
        /* else: other ESC <single byte> sequence — already consumed. */
    }
    flush();
    close_span();
    return html;
}

/* ------------------------------------------------------------------------ */
/* Detection                                                                */
/* ------------------------------------------------------------------------ */

static inline bool path_has(const char *bin) {
    const char *path = getenv("PATH");
    if (!path) return false;
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < 256) {
            char buf[512];
            int wn = snprintf(buf, sizeof(buf), "%.*s/%s", (int)len, p, bin);
            if (wn > 0 && wn < (int)sizeof(buf) && access(buf, X_OK) == 0) return true;
        }
        p = colon ? colon + 1 : p + len;
    }
    return false;
}

static inline HLTool detect_tool(bool force_off) {
    if (force_off) return HLTool::None;
    if (getenv("MDVIEW_NO_HIGHLIGHT")) return HLTool::None;
    if (path_has("bat"))              return HLTool::Bat;
    if (path_has("pygmentize"))       return HLTool::Pygmentize;
    if (path_has("highlight"))        return HLTool::Highlight;
    if (path_has("source-highlight")) return HLTool::SourceHighlight;
    return HLTool::None;
}

/* ------------------------------------------------------------------------ */
/* Highlighter                                                              */
/* ------------------------------------------------------------------------ */

class Highlighter {
public:
    typedef void (*MilestoneFn)(mdv_window *win, void *userdata);

    Highlighter() = default;
    Highlighter(const Highlighter&) = delete;
    Highlighter& operator=(const Highlighter&) = delete;

    ~Highlighter() {
        /* The destructor only closes our pipe fds and reaps stragglers.
           Don't call mdv_window_unwatch_fd here — the window may already
           have been destroyed by the time the App goes out of scope.
           For mid-run shutdown call stop() explicitly. */
        for (auto &job : jobs_) {
            if (job.stdout_fd >= 0) { close(job.stdout_fd); job.stdout_fd = -1; }
            if (job.pid > 0 && !job.done) {
                kill(job.pid, SIGKILL);
                waitpid(job.pid, NULL, 0);
                job.pid = -1;
            }
        }
    }

    /* Cancel everything in flight and detach from the window. Safe to call
       while the window is still alive — unlike the destructor, this DOES
       unwatch fds. */
    void stop() {
        for (auto &job : jobs_) {
            if (job.stdout_fd >= 0) {
                if (win_) mdv_window_unwatch_fd(win_, job.stdout_fd);
                close(job.stdout_fd);
                job.stdout_fd = -1;
            }
            if (job.pid > 0 && !job.done) {
                kill(job.pid, SIGKILL);
                waitpid(job.pid, NULL, 0);
                job.pid = -1;
            }
        }
        win_ = nullptr;
    }

    void set_tool(HLTool t) { tool_ = t; }
    HLTool tool() const     { return tool_; }
    bool   active() const   { return tool_ != HLTool::None; }
    bool   all_done() const { return all_done_; }
    bool   any_done() const { return first_fired_; }

    /* Hand over a set of code blocks and start highlighting up to MAX_INFLIGHT
       in parallel. on_milestone fires:
         - once after the first min(MAX_INFLIGHT, n_highlightable) jobs finish,
         - once more after every job has finished (skipped if the first call
           already covered all jobs). */
    void start(mdv_window *win,
               std::vector<CodeBlock> blocks,
               MilestoneFn on_milestone,
               void *userdata) {
        win_           = win;
        on_milestone_  = on_milestone;
        userdata_      = userdata;

        /* Reset run state — start() may be called again after a previous run
           (e.g. on dark-mode toggle or live reload). Without this, stale
           n_done_ / first_fired_ / all_done_ from the prior run would either
           fire milestones immediately or never. */
        jobs_.clear();
        n_highlightable_ = 0;
        n_done_          = 0;
        in_flight_       = 0;
        first_fired_     = false;
        all_done_        = false;

        jobs_.reserve(blocks.size());
        for (auto &b : blocks) {
            Job j;
            j.content_start = b.content_start;
            j.content_end   = b.content_end;
            j.lang          = std::move(b.lang);
            j.content       = std::move(b.raw_content);
            j.parent        = this;
            jobs_.push_back(std::move(j));
        }

        bool supports_auto = (tool_ == HLTool::Pygmentize);
        n_highlightable_ = 0;
        for (auto &job : jobs_) {
            bool can = tool_ != HLTool::None
                    && !job.content.empty()
                    && job.content.size() <= MAX_BLOCK_BYTES
                    && (supports_auto || !job.lang.empty());
            if (!can) job.done = true;
            else      n_highlightable_++;
        }

        if (n_highlightable_ == 0) {
            first_fired_ = true;
            all_done_    = true;
            if (on_milestone_) on_milestone_(win_, userdata_);
            return;
        }
        for (int i = 0; i < MAX_INFLIGHT; i++) launch_next();
    }

    /* Build a new HTML string by substituting <span>-highlighted output into
       each block's content range. Blocks that haven't been highlighted (yet
       or ever) keep their original content. */
    std::string build_html(const std::string &orig) const {
        std::string out;
        out.reserve(orig.size() + orig.size() / 4);
        size_t pos = 0;
        for (const auto &job : jobs_) {
            out.append(orig, pos, job.content_start - pos);
            if (!job.highlighted.empty()) {
                out += job.highlighted;
            } else {
                out.append(orig, job.content_start, job.content_end - job.content_start);
            }
            pos = job.content_end;
        }
        out.append(orig, pos, std::string::npos);
        return out;
    }

private:
    static const int    MAX_INFLIGHT    = 4;
    static const size_t MAX_BLOCK_BYTES = 1u << 20;  /* 1 MiB */

    struct Job {
        size_t      content_start = 0;
        size_t      content_end   = 0;
        std::string lang;
        std::string content;        /* input to highlighter */
        std::string output;         /* raw ANSI bytes from child */
        std::string highlighted;    /* HTML span result, on success */
        pid_t       pid       = -1;
        int         stdout_fd = -1;
        bool        launched  = false;
        bool        done      = false;
        Highlighter *parent   = nullptr;
    };

    void launch_next() {
        if (in_flight_ >= MAX_INFLIGHT) return;
        for (auto &job : jobs_) {
            if (job.launched || job.done) continue;
            if (launch_job(job)) return;  /* success — one slot filled */
            /* Couldn't launch this one; mark it done and try the next. */
            job.done = true;
            n_done_++;
            check_milestones();
        }
    }

    bool launch_job(Job &job) {
        int out_pipe[2];
        if (pipe(out_pipe) < 0) return false;

        /* Stage stdin in a temp file rather than a pipe. With a pipe the
           parent's synchronous write can deadlock against a child whose
           own stdout pipe is full — both ends blocked. A regular file lets
           the child consume input at its own pace regardless of how fast
           we drain its stdout. /tmp is typically tmpfs (memory-backed). */
        char tmpl[] = "/tmp/mdview-hl.XXXXXX";
        int tfd = mkstemp(tmpl);
        if (tfd < 0) { close(out_pipe[0]); close(out_pipe[1]); return false; }
        unlink(tmpl);  /* freed on last close */

        size_t total = 0;
        while (total < job.content.size()) {
            ssize_t n = write(tfd, job.content.data() + total,
                              job.content.size() - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                close(tfd); close(out_pipe[0]); close(out_pipe[1]);
                return false;
            }
            total += (size_t)n;
        }
        lseek(tfd, 0, SEEK_SET);

        pid_t pid = fork();
        if (pid < 0) {
            close(tfd); close(out_pipe[0]); close(out_pipe[1]);
            return false;
        }

        if (pid == 0) {
            /* Child: rewire fds and exec. */
            dup2(tfd, 0);
            dup2(out_pipe[1], 1);
            close(tfd); close(out_pipe[0]); close(out_pipe[1]);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, 2); close(devnull); }

            const char *lang = job.lang.empty() ? nullptr : job.lang.c_str();
            switch (tool_) {
            case HLTool::Bat:
                if (lang) execlp("bat", "bat", "--color=always", "--plain",
                                  "--paging=never", "--decorations=never",
                                  "--theme=GitHub", "-l", lang, (char*)NULL);
                break;
            case HLTool::Pygmentize:
                if (lang) execlp("pygmentize", "pygmentize", "-f", "terminal16m",
                                  "-O", "style=friendly", "-l", lang, (char*)NULL);
                else      execlp("pygmentize", "pygmentize", "-f", "terminal16m",
                                  "-O", "style=friendly", "-g", (char*)NULL);
                break;
            case HLTool::Highlight:
                if (lang) execlp("highlight", "highlight",
                                  "--out-format=truecolor", "--style=github",
                                  "--syntax", lang, (char*)NULL);
                break;
            case HLTool::SourceHighlight:
                if (lang) execlp("source-highlight",
                                  "source-highlight", "--out-format=esc256",
                                  "--src-lang", lang, (char*)NULL);
                break;
            default: break;
            }
            _exit(127);
        }

        /* Parent. */
        close(tfd);
        close(out_pipe[1]);

        int flags = fcntl(out_pipe[0], F_GETFL, 0);
        fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

        job.pid       = pid;
        job.stdout_fd = out_pipe[0];
        job.launched  = true;

        if (mdv_window_watch_fd(win_, out_pipe[0], &Highlighter::on_data_static, &job) != 0) {
            /* Watcher table is full — abandon this job. */
            close(out_pipe[0]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            job.pid = -1;
            job.stdout_fd = -1;
            return false;
        }

        in_flight_++;
        return true;
    }

    static void on_data_static(mdv_window *win, int fd, void *ud) {
        Job *job = (Job*)ud;
        job->parent->on_data(win, fd, job);
    }

    void on_data(mdv_window *win, int fd, Job *job) {
        char buf[4096];
        for (;;) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) { job->output.append(buf, (size_t)n); continue; }
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                break;  /* other error → finalize */
            }
            break;  /* EOF */
        }
        /* Finalize. */
        mdv_window_unwatch_fd(win, fd);
        close(fd);
        job->stdout_fd = -1;

        int status = 0;
        if (job->pid > 0) waitpid(job->pid, &status, 0);
        job->pid = -1;

        bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && !job->output.empty();
        if (ok) job->highlighted = ansi_to_html_spans(job->output);

        job->done = true;
        in_flight_--;
        n_done_++;

        launch_next();
        check_milestones();
    }

    void check_milestones() {
        int first_threshold = (MAX_INFLIGHT < n_highlightable_) ? MAX_INFLIGHT : n_highlightable_;
        if (!first_fired_ && n_done_ >= first_threshold) {
            first_fired_ = true;
            if (n_done_ == n_highlightable_) all_done_ = true;
            if (on_milestone_) on_milestone_(win_, userdata_);
            return;
        }
        if (!all_done_ && n_done_ == n_highlightable_) {
            all_done_ = true;
            if (on_milestone_) on_milestone_(win_, userdata_);
        }
    }

    HLTool                 tool_           = HLTool::None;
    std::vector<Job>       jobs_;
    int                    n_highlightable_ = 0;
    int                    n_done_          = 0;
    int                    in_flight_       = 0;
    bool                   first_fired_     = false;
    bool                   all_done_        = false;
    mdv_window            *win_             = nullptr;
    MilestoneFn            on_milestone_    = nullptr;
    void                  *userdata_        = nullptr;
};

}  /* namespace mdv */

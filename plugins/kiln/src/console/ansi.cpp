#include "console/ansi.h"

#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string_view>

#ifdef __linux__
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace kiln::console {

bool is_tty(int fd) {
#ifdef __linux__
    return ::isatty(fd) == 1;
#else
    (void)fd;
    return false;
#endif
}

int term_width() {
#ifdef __linux__
    struct winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return static_cast<int>(ws.ws_col);
#endif
    return 0;
}

bool utf8_locale() {
    const char* l = std::setlocale(LC_CTYPE, nullptr);
    if (!l)
        return false;
    std::string_view s(l);
    return s.find("UTF-8") != std::string_view::npos ||
           s.find("utf8") != std::string_view::npos;
}

namespace {
std::atomic<uint32_t> g_sigwinch{0};
void sigwinch_handler(int) { g_sigwinch.fetch_add(1, std::memory_order_relaxed); }
} // namespace

void install_sigwinch_handler() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, nullptr);
}

bool sigwinch_pending() {
    return g_sigwinch.exchange(0, std::memory_order_relaxed) > 0;
}

Colors detect_colors() {
    Colors c;
    if (const char* nc = std::getenv("NO_COLOR"); nc && *nc) {
        c.enabled = false;
        return c;
    }
    if (const char* fc = std::getenv("FORCE_COLOR"); fc && *fc) {
        c.enabled = true;
        return c;
    }
    c.enabled = is_tty(STDOUT_FILENO) && !(std::getenv("TERM") &&
                                           std::string_view(std::getenv("TERM")) == "dumb");
    return c;
}

static std::string code(const Colors& c, const char* open) {
    if (!c.enabled)
        return "";
    return std::string("\x1b[") + open;
}

std::string Colors::red(std::string_view s) const {
    return code(*this, "31m") + std::string(s) + code(*this, "0m");
}
std::string Colors::green(std::string_view s) const {
    return code(*this, "32m") + std::string(s) + code(*this, "0m");
}
std::string Colors::yellow(std::string_view s) const {
    return code(*this, "33m") + std::string(s) + code(*this, "0m");
}
std::string Colors::dim(std::string_view s) const {
    return code(*this, "2m") + std::string(s) + code(*this, "0m");
}
std::string Colors::bold(std::string_view s) const {
    return code(*this, "1m") + std::string(s) + code(*this, "0m");
}
std::string Colors::cyan(std::string_view s) const {
    return code(*this, "36m") + std::string(s) + code(*this, "0m");
}

std::string fmt_duration_ms(uint64_t ms) {
    if (ms < 1000)
        return std::to_string(ms) + " ms";
    if (ms < 60000) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f s", static_cast<double>(ms) / 1000.0);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%llum%llus",
             static_cast<unsigned long long>(ms / 60000),
             static_cast<unsigned long long>((ms / 1000) % 60));
    return buf;
}

std::string fmt_elapsed(uint64_t start_ms, uint64_t now_ms) {
    return fmt_duration_ms(now_ms - start_ms);
}

std::string teamcity_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
        case '|': out += "||"; break;
        case '\'': out += "|'"; break;
        case '\n': out += "|n"; break;
        case '\r': out += "|r"; break;
        case '[': out += "|["; break;
        case ']': out += "|]"; break;
        default: out += ch;
        }
    }
    return out;
}

} // namespace kiln::console

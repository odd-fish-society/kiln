#pragma once

#include <atomic>
#include <string>

namespace kiln::console {

bool is_tty(int fd);
// Terminal width via TIOCGWINSZ; 0 when unavailable.
int term_width();
bool utf8_locale();

// Installs a SIGWINCH handler that bumps an atomic counter (no async-unsafe
// calls in the handler itself). The renderer polls the counter.
void install_sigwinch_handler();
bool sigwinch_pending(); // consume-and-reset

struct Colors {
    bool enabled = true;
    std::string red(std::string_view s) const;
    std::string green(std::string_view s) const;
    std::string yellow(std::string_view s) const;
    std::string dim(std::string_view s) const;
    std::string bold(std::string_view s) const;
    std::string cyan(std::string_view s) const;
};

Colors detect_colors(); // NO_COLOR / FORCE_COLOR / tty

std::string fmt_duration_ms(uint64_t ms);   // "412 ms" / "1.2 s" / "1m12s"
std::string fmt_elapsed(uint64_t start_ms, uint64_t now_ms); // "1m12s"

// Escape a string for TeamCity service messages.
std::string teamcity_escape(std::string_view s);

} // namespace kiln::console

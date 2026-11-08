#include "console/renderer.h"

#include <algorithm>
#include <cstdio>

#include "core/util.h"

namespace kiln::console {

ProgressMode parse_progress_mode(const std::string& s) {
    if (s == "fancy")
        return ProgressMode::Fancy;
    if (s == "plain")
        return ProgressMode::Plain;
    if (s == "none")
        return ProgressMode::None;
    return ProgressMode::Fancy;
}

namespace {

const char* kSpinner = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() || !out.empty())
                out.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

} // namespace

Renderer::Renderer(Options opts) : opts_(std::move(opts)), colors_(detect_colors()) {
    utf8_ = utf8_locale();
    teamcity_ = std::getenv("TEAMCITY_VERSION") != nullptr;
    width_ = term_width();
    if (width_ <= 0)
        width_ = 80;
    if (opts_.color)
        colors_.enabled = true;
    install_sigwinch_handler();
    start_ms_ = util::now_ms();
    thread_ = std::thread([this] {
        for (;;) {
            {
                std::unique_lock lock(mu_);
                // Wake on event arrival (condvar); periodic timeout only for
                // SIGWINCH polling, plain-mode heartbeats, and the 33 ms
                // repaint throttle.
                bool shutdown_now = false;
                cv_.wait_for(lock, std::chrono::milliseconds(next_timeout_ms()), [&] {
                    return shutdown_requested_ || !queue_.empty();
                });
                if (queue_.empty() && shutdown_requested_) {
                    shutdown_now = true;
                }
                if (!shutdown_now) {
                    // Drain the batch.
                    while (!queue_.empty()) {
                        Item item = std::move(queue_.front());
                        queue_.pop_front();
                        if (item.shutdown) {
                            shutdown_now = true;
                            break;
                        }
                        // Unlock while consuming so pushes can keep arriving.
                        const uint64_t ticket = item.fn_ticket;
                        lock.unlock();
                        consume(std::move(item));
                        lock.lock();
                        if (ticket) {
                            fn_done_ = ticket;
                            fn_done_cv_.notify_all();
                        }
                    }
                }
                if (shutdown_now) {
                    // Drain anything left, force a final repaint, then done.
                    while (!queue_.empty()) {
                        Item leftover = std::move(queue_.front());
                        queue_.pop_front();
                        if (leftover.shutdown)
                            continue;
                        const uint64_t ticket = leftover.fn_ticket;
                        consume(std::move(leftover));
                        if (ticket) {
                            fn_done_ = ticket;
                            fn_done_cv_.notify_all();
                        }
                    }
                    if (dirty_) {
                        // Flush the final state, still honoring the ≥33 ms
                        // repaint floor so the cadence invariant holds exactly.
                        uint64_t now = util::now_ms();
                        if (now - last_repaint_ms_ < 33)
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(33 - (now - last_repaint_ms_)));
                        dirty_ = false;
                        repaint();
                    }
                    break;
                }
            }
            periodic_tick();
            maybe_repaint();
        }
    });
}

Renderer::~Renderer() {
    {
        std::lock_guard lock(mu_);
        shutdown_requested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void Renderer::push(Event e) {
    std::lock_guard lock(mu_);
    Item item;
    item.ev = std::move(e);
    queue_.push_back(std::move(item));
    cv_.notify_all();
}

void Renderer::note(std::string line) {
    std::lock_guard lock(mu_);
    Item item;
    item.note = true;
    item.note_text = std::move(line);
    queue_.push_back(std::move(item));
    cv_.notify_all();
}

void Renderer::finish() {
    {
        std::lock_guard lock(mu_);
        Item item;
        item.shutdown = true;
        queue_.push_back(std::move(item));
        cv_.notify_all();
    }
    if (thread_.joinable())
        thread_.join();
}

void Renderer::run_on_consumer(std::function<void()> fn) {
    if (!thread_.joinable() || std::this_thread::get_id() == thread_.get_id()) {
        fn();
        return;
    }
    std::unique_lock lock(mu_);
    Item item;
    item.fn = std::move(fn);
    item.fn_ticket = ++fn_issued_;
    const uint64_t ticket = item.fn_ticket;
    queue_.push_back(std::move(item));
    cv_.notify_all();
    fn_done_cv_.wait(lock, [&] { return fn_done_ >= ticket; });
}

void Renderer::teardown_region() {
    run_on_consumer([this] {
        clear_region();
        torn_down_ = true;
        std::fflush(stdout);
    });
}

void Renderer::print_summary(uint64_t done, uint64_t cached, uint64_t failed,
                             uint64_t skipped, uint64_t elapsed_ms) {
    run_on_consumer([=, this] { print_summary_now(done, cached, failed, skipped, elapsed_ms); });
}

void Renderer::print_summary_now(uint64_t done, uint64_t cached, uint64_t failed,
                                 uint64_t skipped, uint64_t elapsed_ms) {
    if (failed == 0) {
        std::string line = (utf8_ ? "✓ " : "ok ") + std::to_string(done) + " tasks, " +
                           std::to_string(cached) + " cached";
        if (skipped)
            line += ", " + std::to_string(skipped) + " skipped";
        line += ", " + fmt_duration_ms(elapsed_ms);
        print_plain(colors_.green(line));
    } else {
        std::string line = (utf8_ ? "✗ " : "FAIL ") + std::to_string(failed) + " failed:";
        print_plain(colors_.red(line));
        for (auto& f : failures_)
            print_plain(colors_.red("  " + f));
    }
}

size_t Renderer::visible_len(std::string_view s) {
    size_t len = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b') {
            while (i < s.size() && s[i] != 'm')
                ++i;
            continue;
        }
        ++len;
    }
    return len;
}

std::string Renderer::truncate(std::string line, int width) {
    if (width <= 0)
        return line;
    // Truncate on visible length; over-trim by ANSI slack is avoided by the loop.
    std::string out;
    size_t vis = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\x1b') {
            out += line[i];
            while (i < line.size() && line[i] != 'm') {
                ++i;
                out += line[i];
            }
            continue;
        }
        if (vis >= static_cast<size_t>(width - 1)) {
            out += "…";
            break;
        }
        out += line[i];
        ++vis;
    }
    return out;
}

void Renderer::print_plain(const std::string& line) {
    std::string out = line;
    if (opts_.mode == ProgressMode::Fancy && !torn_down_) {
        scroll_line(out);
        return;
    }
    std::fputs(out.c_str(), stdout);
    std::fputc('\n', stdout);
    last_output_ms_ = util::now_ms();
    if (teamcity_)
        std::printf("##teamcity[progressMessage '%s']\n", teamcity_escape(out).c_str());
    std::fflush(stdout);
}

void Renderer::scroll_line(std::string line) {
    scroll_.push_back(truncate(std::move(line), width_));
    if (scroll_.size() > kScrollMax)
        scroll_.erase(scroll_.begin());
    dirty_ = true;
}

void Renderer::clear_region() {
    if (region_rows_ == 0)
        return;
    std::string out;
    out += "\x1b[" + std::to_string(region_rows_) + "A";
    for (size_t i = 0; i < region_rows_; ++i)
        out += "\x1b[2K\n";
    out += "\x1b[" + std::to_string(region_rows_) + "A";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    region_rows_ = 0;
}

uint64_t Renderer::next_timeout_ms() const {
    uint64_t t = 50; // periodic SIGWINCH poll + heartbeat granularity
    if (dirty_ && opts_.mode == ProgressMode::Fancy) {
        uint64_t elapsed = util::now_ms() - last_repaint_ms_;
        if (elapsed < 33)
            t = std::min<uint64_t>(t, 33 - elapsed);
        else
            t = std::min<uint64_t>(t, 1);
    }
    return t;
}

void Renderer::maybe_repaint() {
    if (!dirty_ || opts_.mode != ProgressMode::Fancy)
        return;
    uint64_t now = util::now_ms();
    if (now - last_repaint_ms_ < 33)
        return; // throttle; next_timeout_ms schedules the wake
    dirty_ = false;
    repaint();
}

void Renderer::periodic_tick() {
    if (sigwinch_pending()) {
        width_ = term_width();
        if (width_ <= 0)
            width_ = 80;
        dirty_ = true;
    }
    heartbeat_if_idle();
}

void Renderer::repaint() {
    if (opts_.mode != ProgressMode::Fancy || !saw_any_event_)
        return;
    last_repaint_ms_ = util::now_ms();
    repaint_count_.fetch_add(1, std::memory_order_relaxed);
    repaint_times_.push_back(last_repaint_ms_);
    std::string out;
    out += "\x1b[?2026h"; // DEC synchronized output (harmless where unsupported)
    if (region_rows_ > 0)
        out += "\x1b[" + std::to_string(region_rows_) + "A";
    size_t rows = scroll_.size() + 2;
    for (size_t i = 0; i < rows; ++i) {
        out += "\x1b[2K";
        if (i < scroll_.size()) {
            out += scroll_[i];
        } else if (i == scroll_.size()) {
            // Counter line.
            char buf[256];
            std::string total_s = std::to_string(tasks_total_);
            std::string last = last_started_;
            std::string spinner;
            if (graph_growing_)
                spinner = std::string(1, kSpinner[spinner_frame_ % 10]);
            snprintf(buf, sizeof(buf), "%s[%llu/%s%s]%s %s",
                     colors_.dim("").c_str(),
                     static_cast<unsigned long long>(tasks_done_), total_s.c_str(),
                     graph_growing_ ? "…" : "", spinner.c_str(), last.c_str());
            out += truncate(buf, width_);
        } else {
            // Bar line.
            int barw = std::max(10, width_ - 32);
            double frac = tasks_total_ ? static_cast<double>(tasks_done_) /
                                             static_cast<double>(tasks_total_)
                                       : 0.0;
            frac = std::clamp(frac, 0.0, 1.0);
            int filled = static_cast<int>(frac * barw + 0.5);
            std::string full = "█", empty = "░";
            if (!utf8_) {
                full = "#";
                empty = "-";
            }
            std::string bar = "[";
            for (int i = 0; i < barw; ++i)
                bar += (i < filled) ? full : empty;
            bar += "]";
            char buf[320];
            snprintf(buf, sizeof(buf), "%s %3d%% %2llu running %s", bar.c_str(),
                     static_cast<int>(frac * 100),
                     static_cast<unsigned long long>(running_),
                     fmt_elapsed(start_ms_, util::now_ms()).c_str());
            out += truncate(buf, width_);
        }
        out += "\n";
    }
    out += "\x1b[?2026l";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    region_rows_ = rows;
}

void Renderer::consume(Item&& item) {
    if (item.shutdown)
        return;
    if (item.fn) {
        item.fn(); // the caller records fn_ticket under mu_ (it may hold it now)
        return;
    }
    if (item.note) {
        if (opts_.mode == ProgressMode::Fancy) {
            scroll_line(std::move(item.note_text));
        } else if (opts_.mode == ProgressMode::Plain) {
            print_plain(item.note_text);
        }
        return;
    }
    handle_event(item.ev);
}

void Renderer::handle_event(const Event& e) {
    switch (e.kind) {
    case Event::GraphGrew:
        saw_any_event_ = true;
        graph_growing_ = true;
        tasks_total_ = e.total;
        dirty_ = true;
        break;
    case Event::TaskStarted:
        saw_any_event_ = true;
        build_active_ = true;
        ++running_;
        last_started_ = e.id.display();
        if (opts_.mode == ProgressMode::Fancy)
            dirty_ = true;
        break;
    case Event::LogBlock:
        saw_any_event_ = true;
        pending_logs_[e.id.display()] += e.text;
        break;
    case Event::TaskFinished: {
        saw_any_event_ = true;
        build_active_ = true;
        tasks_done_ = e.done;
        tasks_total_ = e.total;
        running_ = e.running;
        std::string log;
        auto it = pending_logs_.find(e.id.display());
        if (it != pending_logs_.end()) {
            log = std::move(it->second);
            pending_logs_.erase(it);
        }
        std::string status = task_state_name(e.state);
        if (e.state == TaskState::Failed) {
            any_failure_ = true;
            failures_.push_back(e.id.display());
            if (opts_.mode == ProgressMode::Fancy) {
                scroll_line(colors_.red("[fail] " + e.id.display() +
                                        (e.ms ? "  " + fmt_duration_ms(e.ms) : "")));
                for (auto& line : split_lines(log))
                    scroll_line(colors_.red("  | " + line));
            } else {
                print_plain(colors_.red("[fail] " + e.id.display() +
                                        (e.ms ? "  " + fmt_duration_ms(e.ms) : "")));
                for (auto& line : split_lines(log))
                    print_plain(colors_.red("  | " + line));
            }
        } else {
            if (opts_.mode == ProgressMode::Fancy) {
                std::string line = std::string("[") + status + "] " + e.id.display();
                if (e.ms)
                    line += "  " + fmt_duration_ms(e.ms);
                scroll_line(e.state == TaskState::SkippedUpToDate ? colors_.dim(line)
                                                                  : colors_.green(line));
                for (auto& l : split_lines(log))
                    scroll_line(colors_.dim("  | " + l));
            } else if (opts_.mode == ProgressMode::Plain) {
                char buf[512];
                snprintf(buf, sizeof(buf), "[%llu/%llu] %s %s%s", 
                         static_cast<unsigned long long>(e.done),
                         static_cast<unsigned long long>(e.total), status.c_str(),
                         e.id.display().c_str(),
                         e.ms ? ("  " + fmt_duration_ms(e.ms)).c_str() : "");
                print_plain(buf);
                for (auto& l : split_lines(log))
                    print_plain("  | " + l);
            } else {
                // none: only failures print (handled above)
            }
        }
        if (opts_.mode == ProgressMode::Fancy)
            dirty_ = true;
        break;
    }
    case Event::BuildDone:
        build_active_ = false;
        graph_growing_ = false;
        tasks_done_ = e.done;
        tasks_total_ = e.total;
        if (opts_.mode == ProgressMode::Fancy)
            dirty_ = true;
        break;
    }
    spinner_frame_ = (spinner_frame_ + 1) % 10;
}

void Renderer::heartbeat_if_idle() {
    if (opts_.mode != ProgressMode::Plain || !build_active_ || !saw_any_event_)
        return;
    uint64_t now = util::now_ms();
    if (now - last_output_ms_ >= 5000) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[%llu/%llu] ... building %s",
                 static_cast<unsigned long long>(tasks_done_),
                 static_cast<unsigned long long>(tasks_total_),
                 fmt_elapsed(start_ms_, now).c_str());
        print_plain(buf);
    }
}

} // namespace kiln::console

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "console/ansi.h"
#include "core/scheduler.h"

namespace kiln::console {

enum class ProgressMode { Fancy, Plain, None };

ProgressMode parse_progress_mode(const std::string& s);

// Owns stdout. Tasks NEVER print directly — all output flows through events.
class Renderer {
public:
    struct Options {
        ProgressMode mode = ProgressMode::Fancy; // resolved by caller (tty detection)
        bool color = true;
        bool verbose = false;
    };

    explicit Renderer(Options opts);
    ~Renderer();

    void push(Event e);          // thread-safe (scheduler workers)
    void note(std::string line); // informational line above the progress region
    void finish();               // drain queue, tear down region
    // Clears the pinned region (always call on exit — never leave a dead bar).
    void teardown_region();
    // Final summary line(s): "✓ N tasks, M cached, T" or "✗ K failed:" + list.
    void print_summary(uint64_t done, uint64_t cached, uint64_t failed,
                       uint64_t skipped, uint64_t elapsed_ms);
    bool any_failure() const { return any_failure_; }
    uint64_t tasks_done() const { return tasks_done_; }
    uint64_t tasks_total() const { return tasks_total_; }

    // Diagnostics: repaint cadence (acceptance test asserts ≥20 repaints/s
    // under continuous events, ≤1 per 33 ms window, zero when idle).
    uint64_t repaint_count() const { return repaint_count_; }
    std::vector<uint64_t> repaint_times_ms() const { return repaint_times_; }

private:
    struct Item {
        Event ev;
        bool note = false;
        std::string note_text;
        bool shutdown = false;
    };

    void consume(Item&& item);
    void handle_event(const Event& e);
    void scroll_line(std::string line);
    void repaint();
    void maybe_repaint();        // throttled to ≥33 ms between repaints
    void periodic_tick();        // SIGWINCH poll + plain-mode heartbeat
    uint64_t next_timeout_ms() const;
    void clear_region();
    void print_plain(const std::string& line);
    void heartbeat_if_idle();
    static size_t visible_len(std::string_view s);
    static std::string truncate(std::string line, int width);

    Options opts_;
    console::Colors colors_;
    bool utf8_ = false;
    bool teamcity_ = false;
    int width_ = 80;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::thread thread_;
    bool shutdown_requested_ = false;

    // Fancy state.
    std::vector<std::string> scroll_;       // ring buffer of scroll lines
    static constexpr size_t kScrollMax = 200;
    size_t region_rows_ = 0;                // lines currently on screen (scroll+pinned)
    bool graph_growing_ = false;
    size_t spinner_frame_ = 0;

    // Progress state.
    uint64_t tasks_done_ = 0, tasks_total_ = 0, running_ = 0;
    uint64_t start_ms_ = 0;
    std::string last_started_;
    bool build_active_ = false;
    bool any_failure_ = false;
    std::vector<std::string> failures_;
    std::map<std::string, std::string> pending_logs_; // task display → buffered log
    uint64_t last_output_ms_ = 0;
    bool saw_any_event_ = false;
    bool torn_down_ = false;
    // Fancy repaint throttle (spec §3a): condvar wake on events, ≥33 ms floor.
    bool dirty_ = false;
    uint64_t last_repaint_ms_ = 0;
    std::atomic<uint64_t> repaint_count_{0};
    std::vector<uint64_t> repaint_times_; // consumer-thread only
};

} // namespace kiln::console

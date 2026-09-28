#include "core/scheduler.h"

#include <algorithm>
#include <deque>

#include "core/digest.h"
#include "core/util.h"

namespace kiln {

Scheduler::Scheduler(fs::path workspace_root, fs::path kiln_dir, FileHashCache& hash_cache,
                     TraceStore& traces,
                     const std::function<TaskFn(const std::string&)>& rule_lookup,
                     std::function<void(Event)> sink, Options options)
    : workspace_root_(std::move(workspace_root)), kiln_dir_(std::move(kiln_dir)),
      hash_cache_(hash_cache), traces_(traces), rule_lookup_(rule_lookup),
      sink_(std::move(sink)), options_(std::move(options)) {
    if (options_.jobs <= 0)
        options_.jobs = static_cast<int>(std::thread::hardware_concurrency());
    if (options_.jobs < 1)
        options_.jobs = 1;
}

uint64_t done_count(const BuildStats& s) {
    return s.succeeded + s.cached + s.failed + s.skipped;
}

Scheduler::~Scheduler() {
    {
        std::lock_guard lock(mu_);
        shutdown_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable())
            t.join();
}

Result<size_t> Scheduler::add(TaskDecl decl) {
    auto th = decl.id.stable_hash();
    auto existing = graph_.find(th);
    if (existing)
        return Result<size_t>::success(*existing);

    auto idx_res = graph_.add(std::move(decl));
    if (!idx_res.ok())
        return Result<size_t>::fail(idx_res.error);
    size_t idx = *idx_res;

    runtime_.resize(graph_.size());
    session_.resize(graph_.size());
    auto& node = graph_.nodes()[idx];
    auto& rt = runtime_[idx];

    size_t pending = 0;
    bool upstream_failed = false;
    std::optional<size_t> failed_dep;
    for (auto d : node.deps) {
        auto& drt = runtime_[d];
        if (drt.terminal) {
            if (drt.upstream_failed) {
                upstream_failed = true;
                failed_dep = d;
            }
        } else {
            ++pending;
        }
    }
    rt.pending_deps = pending;

    if (upstream_failed && options_.keep_going) {
        rt.terminal = true;
        rt.upstream_failed = true;
        session_[idx].state = TaskState::SkippedUpstreamFailed;
        ++stats_.skipped;
        WhyRecord wr;
        wr.task_display = node.decl.id.display();
        wr.state = TaskState::SkippedUpstreamFailed;
        wr.kind = "upstream_failed";
        wr.dep_display = failed_dep ? graph_.nodes()[*failed_dep].decl.id.display() : "";
        why_.push_back(std::move(wr));
    } else if (pending == 0 && !upstream_failed) {
        std::lock_guard lock(mu_);
        ready_.push_back(idx);
    }
    if (sink_) {
        Event e;
        e.kind = Event::GraphGrew;
        e.id = node.decl.id;
        e.done = static_cast<uint32_t>(done_count(stats_));
        e.total = static_cast<uint32_t>(graph_.size());
        sink_(std::move(e));
    }
    return Result<size_t>::success(idx);
}

bool Scheduler::pop_ready_probe_locked() const {
    for (size_t idx : ready_) {
        const auto& node = graph_.nodes()[idx];
        const std::string& pool = node.decl.pool;
        int cap = options_.jobs;
        if (!pool.empty()) {
            auto it = options_.pools.find(pool);
            if (it != options_.pools.end())
                cap = std::max(1, it->second);
        }
        auto it = pool_in_flight_.find(pool);
        int in_flight = it == pool_in_flight_.end() ? 0 : it->second;
        if (in_flight < cap)
            return true;
    }
    return false;
}

bool Scheduler::pop_ready_locked(size_t& out_index) {
    for (size_t qi = 0; qi < ready_.size(); ++qi) {
        size_t idx = ready_[qi];
        const auto& node = graph_.nodes()[idx];
        const std::string& pool = node.decl.pool;
        int cap = options_.jobs;
        if (!pool.empty()) {
            auto it = options_.pools.find(pool);
            if (it != options_.pools.end())
                cap = std::max(1, it->second);
        }
        int& in_flight = pool_in_flight_[pool];
        if (in_flight < cap) {
            out_index = idx;
            ready_.erase(ready_.begin() + static_cast<long>(qi));
            ++in_flight;
            ++running_;
            return true;
        }
    }
    return false;
}

void Scheduler::worker_loop() {
    Executor executor(workspace_root_, kiln_dir_, hash_cache_, traces_, rule_lookup_);
    for (;;) {
        size_t idx;
        bool got = false;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [&] {
                if (shutdown_)
                    return true;
                if (cancelled_ || running_ >= options_.jobs)
                    return false;
                return pop_ready_probe_locked();
            });
            if (shutdown_)
                break;
            got = pop_ready_locked(idx);
        }
        if (!got)
            continue;

        const auto& node = graph_.nodes()[idx];
        session_[idx].state = TaskState::Running;
        if (sink_) {
            Event e;
            e.kind = Event::TaskStarted;
            e.id = node.decl.id;
            e.state = TaskState::Running;
            sink_(std::move(e));
        }

        std::vector<Executor::DepInfo> dep_info;
        for (auto d : node.deps) {
            Executor::DepInfo di;
            di.id = graph_.nodes()[d].decl.id;
            di.outputs_changed = session_[d].outputs_changed;
            if (session_[d].new_trace) {
                if (graph_.nodes()[d].decl.outputs_opaque)
                    di.output_summary = input_summary_hash(session_[d].new_trace->inputs);
                else
                    di.output_summary = output_summary_hash(session_[d].new_trace->outputs);
            }
            dep_info.push_back(std::move(di));
        }

        auto res = executor.run_task(node.decl, dep_info, cancelled_);

        if (!res.log.empty() && sink_) {
            Event e;
            e.kind = Event::LogBlock;
            e.id = node.decl.id;
            e.state = res.state;
            e.text = std::move(res.log);
            sink_(std::move(e));
        }

        {
            std::lock_guard lock(mu_);
            --running_;
            auto it = pool_in_flight_.find(node.decl.pool);
            if (it != pool_in_flight_.end() && it->second > 0)
                --it->second;

            session_[idx].state = res.state;
            if (res.state == TaskState::Succeeded || res.state == TaskState::SkippedUpToDate)
                session_[idx].new_trace = res.trace;
            session_[idx].outputs_changed = res.outputs_changed;

            WhyRecord wr;
            wr.task_display = node.decl.id.display();
            wr.state = res.state;
            wr.kind = res.why_ref.kind;
            wr.path = res.why_ref.path;
            wr.was = res.why_ref.was;
            wr.now = res.why_ref.now;
            wr.dep_display = res.why_ref.dep_display;
            wr.msg = res.why_ref.msg.empty() ? res.why : res.why_ref.msg;
            wr.outputs_changed = res.outputs_changed;
            why_.push_back(std::move(wr));

            runtime_[idx].terminal = true;
            runtime_[idx].upstream_failed = (res.state == TaskState::Failed);

            switch (res.state) {
            case TaskState::Succeeded: ++stats_.succeeded; break;
            case TaskState::SkippedUpToDate: ++stats_.cached; break;
            case TaskState::Failed:
                ++stats_.failed;
                failed_ = true;
                if (!options_.keep_going)
                    cancelled_ = true;
                break;
            default: break;
            }

            for (auto dep_idx : node.dependents) {
                auto& drt = runtime_[dep_idx];
                if (drt.terminal)
                    continue;
                if (res.state == TaskState::Failed && options_.keep_going) {
                    drt.terminal = true;
                    drt.upstream_failed = true;
                    session_[dep_idx].state = TaskState::SkippedUpstreamFailed;
                    ++stats_.skipped;
                } else if (res.state == TaskState::Failed) {
                    // fail-fast: dependents never run; mark skipped for bookkeeping.
                    drt.terminal = true;
                    drt.upstream_failed = true;
                    session_[dep_idx].state = TaskState::SkippedUpstreamFailed;
                    ++stats_.skipped;
                } else {
                    if (--drt.pending_deps == 0)
                        ready_.push_back(dep_idx);
                }
            }
        }

        if (sink_) {
            Event e;
            e.kind = Event::TaskFinished;
            e.id = node.decl.id;
            e.state = res.state;
            e.ms = res.trace ? static_cast<uint64_t>(std::max<int64_t>(0, res.trace->wall_ms))
                             : 0;
            e.done = static_cast<uint32_t>(done_count(stats_));
            e.total = static_cast<uint32_t>(graph_.size());
            e.running = static_cast<uint32_t>(running_);
            sink_(std::move(e));
        }

        cv_.notify_all();
    }
}

Result<void> Scheduler::run() {
    KILN_TRY(graph_.check_cycles());

    {
        std::lock_guard lock(mu_);
        stats_.total = graph_.size();
        if (workers_.empty()) {
            workers_.reserve(static_cast<size_t>(options_.jobs));
            for (int i = 0; i < options_.jobs; ++i)
                workers_.emplace_back([this] { worker_loop(); });
        }
    }
    cv_.notify_all();

    for (;;) {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] {
            if (running_ != 0)
                return false;
            return ready_.empty() || cancelled_;
        });
        bool all_terminal = true;
        for (auto& rt : runtime_)
            if (!rt.terminal)
                all_terminal = false;
        if (all_terminal)
            break;
        if (cancelled_) {
            // fail-fast abort: mark leftovers skipped.
            for (size_t i = 0; i < runtime_.size(); ++i) {
                if (!runtime_[i].terminal) {
                    runtime_[i].terminal = true;
                    runtime_[i].upstream_failed = true;
                    session_[i].state = TaskState::SkippedUpstreamFailed;
                    ++stats_.skipped;
                }
            }
            break;
        }
        // Nothing ready, nothing running, but non-terminal nodes exist — they
        // must be pool-blocked only if running_ > 0, which contradicts the wait.
        // Defensive: mark them skipped (should be unreachable).
        for (size_t i = 0; i < runtime_.size(); ++i) {
            if (!runtime_[i].terminal) {
                runtime_[i].terminal = true;
                session_[i].state = TaskState::SkippedUpstreamFailed;
                ++stats_.skipped;
            }
        }
        break;
    }

    if (sink_) {
        Event e;
        e.kind = Event::BuildDone;
        e.done = static_cast<uint32_t>(done_count(stats_));
        e.total = static_cast<uint32_t>(graph_.size());
        e.state = failed_ ? TaskState::Failed : TaskState::Succeeded;
        sink_(std::move(e));
    }
    return Result<void>::success();
}

} // namespace kiln

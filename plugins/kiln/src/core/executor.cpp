#include "core/executor.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>

#include "core/digest.h"
#include "core/util.h"
#include "nlohmann/json.hpp"

namespace kiln {

using json = nlohmann::json;

namespace {

struct LogBuffer {
    std::mutex mu;
    std::string buf;
    static constexpr size_t kCap = 4 * 1024 * 1024; // 4 MiB

    void append(std::string_view s) {
        std::lock_guard lock(mu);
        if (buf.size() >= kCap)
            return;
        size_t keep = std::min(s.size(), kCap - buf.size());
        buf.append(s.data(), keep);
        if (s.size() > keep && buf.size() == kCap) {
            static const char marker[] = "\n... [log truncated]\n";
            buf.append(marker, sizeof(marker) - 1);
        }
    }
    std::string take() {
        std::lock_guard lock(mu);
        return std::move(buf);
    }
};

struct DepCollector {
    std::mutex mu;
    std::set<std::string> deps;
    void add(std::string_view s) {
        std::lock_guard lock(mu);
        deps.emplace(s);
    }
    std::vector<std::string> sorted() {
        std::lock_guard lock(mu);
        return {deps.begin(), deps.end()};
    }
};

// Whether `p` was last written before `started`. File times come from the
// kernel's coarse clock, which lags the fine one by up to a tick, so a write
// just after the start can carry a slightly earlier time: the margin covers
// it. A whole-second mtime suggests a file system that keeps only seconds
// (or two), where the margin is that granularity.
bool written_before(const fs::path& p, fs::file_time_type started) {
    std::error_code ec;
    auto mtime = fs::last_write_time(p, ec);
    if (ec)
        return false;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();
    auto margin = (ns % 1000000000) == 0 ? std::chrono::milliseconds(2000)
                                         : std::chrono::milliseconds(50);
    return mtime + margin < started;
}

} // namespace

Executor::Executor(fs::path workspace_root, fs::path kiln_dir, FileHashCache& hash_cache,
                   TraceStore& traces,
                   const std::function<TaskFn(const std::string& rule)>& rule_lookup)
    : workspace_root_(std::move(workspace_root)), kiln_dir_(std::move(kiln_dir)),
      hash_cache_(hash_cache), traces_(traces), rule_lookup_(rule_lookup) {}

std::string Executor::params_to_json(const std::map<std::string, std::string>& params) {
    json j = json::object();
    for (auto& [k, v] : params)
        j[k] = v;
    return j.dump();
}

Executor::Result Executor::run_task(const TaskDecl& decl,
                                    const std::vector<DepInfo>& dep_info, bool cancelled) {
    Result out;
    out.state = TaskState::Failed;

    const Hash256 th = decl.id.stable_hash();
    auto old_trace = traces_.get(th);

    std::vector<std::string> dyn_deps = old_trace ? old_trace->dynamic_deps
                                                  : std::vector<std::string>{};

    std::vector<std::pair<TaskId, Hash256>> dep_summaries;
    for (auto& d : dep_info)
        dep_summaries.push_back({d.id, d.output_summary});

    // Every input hash the pre-run digest read: the post-run digest reuses
    // them, so an input edited while the task ran keeps its old hash in the
    // trace and the next build reruns the task.
    std::map<std::string, Hash256> hashed_before;
    auto digest = compute_action_digest(
        decl, dyn_deps,
        [&](const std::string& p) -> kiln::Result<Hash256> {
            auto hv = hash_cache_.hash_of(workspace_root_, p);
            if (hv.ok())
                hashed_before[p] = *hv;
            return hv;
        },
        dep_summaries);
    if (!digest.ok()) {
        out.state = TaskState::Failed;
        out.why = digest.error;
        out.why_ref = Result::WhyRef{.kind = "error", .msg = digest.error};
        return out;
    }

    // Cache hit: verify outputs exist with recorded hashes. always_run tasks
    // (check rules) bypass this: a cached check is a check that never fires.
    bool missing_output = false;
    std::string missing_path;
    if (!decl.always_run && old_trace && old_trace->action_digest == *digest && !cancelled) {
        bool all_ok = true;
        for (auto& o : old_trace->outputs) {
            auto hv = hash_cache_.hash_of(workspace_root_, o.path);
            if (!hv.ok() || *hv != o.hash) {
                all_ok = false;
                missing_output = true;
                missing_path = o.path;
                break;
            }
        }
        if (all_ok) {
            out.state = TaskState::SkippedUpToDate;
            out.trace = old_trace;
            return out;
        }
    }

    // Execute.
    auto fn = rule_lookup_(decl.id.rule);
    if (!fn) {
        out.why = "no such rule: " + decl.id.rule;
        out.why_ref = Result::WhyRef{.kind = "error", .msg = out.why};
        return out;
    }

    fs::path out_dir = kiln_dir_ / "out" / "taskdirs" / th.hex();
    std::error_code ec;
    fs::create_directories(out_dir, ec);

    auto logbuf = std::make_shared<LogBuffer>();
    auto depcollector = std::make_shared<DepCollector>();

    TaskCtx ctx;
    ctx.decl = &decl;
    ctx.workspace_root = workspace_root_;
    ctx.out_dir = out_dir;
    ctx.log = [logbuf](std::string_view s) { logbuf->append(s); };
    ctx.report_dep = [depcollector](std::string_view p) {
        auto rel = util::norm(p);
        depcollector->add(rel);
    };
    ctx.cancelled = [cancelled] { return cancelled; };
    ctx.hash_file = [this](const std::string& rel) {
        return hash_cache_.hash_of(workspace_root_, rel);
    };

    const auto started = fs::file_time_type::clock::now();
    uint64_t t0 = util::now_ms();
    TaskResult tr = fn(ctx);
    uint64_t wall_ms = util::now_ms() - t0;

    out.log = logbuf->take();
    if (!tr.ok) {
        out.state = TaskState::Failed;
        out.trace = old_trace;
        out.why = tr.error.empty() ? "task failed" : tr.error;
        out.why_ref = Result::WhyRef{.kind = "error", .msg = out.why};
        // The reason reaches the console under the [fail] line (the task's
        // log block), not only `.kiln/why.json`.
        if (!tr.error.empty() && out.log.find(tr.error) == std::string::npos) {
            if (!out.log.empty() && out.log.back() != '\n')
                out.log += '\n';
            out.log += tr.error;
            out.log += '\n';
        }
        return out;
    }

    // Hash outputs.
    std::vector<OutputRecord> outputs;
    std::set<std::string> output_set;
    for (auto& p : tr.outputs)
        output_set.insert(p);
    for (auto& p : decl.declared_outputs)
        output_set.insert(p);
    for (auto& p : output_set) {
        auto hv = hash_cache_.hash_of(workspace_root_, p);
        if (!hv.ok()) {
            out.state = TaskState::Failed;
            out.why = "declared output missing after run: " + p + " (" + hv.error + ")";
            out.why_ref = Result::WhyRef{.kind = "error", .msg = out.why};
            return out;
        }
        outputs.push_back({p, *hv});
    }

    auto collected_deps = depcollector->sorted();
    // Validate dynamic deps: exist, inside workspace or an allowlisted SDK root.
    for (auto& p : collected_deps) {
        fs::path abs = workspace_root_ / p;
        if (!fs::exists(abs)) {
            out.state = TaskState::Failed;
            out.why = "reported dep does not exist: " + p;
            out.why_ref = Result::WhyRef{.kind = "error", .msg = out.why};
            return out;
        }
    }

    // The digest the trace keeps covers the dependency set this run
    // discovered, so the next build matches it (no rerun because the set
    // changed). A newly discovered input has no pre-run hash: its current
    // content is trusted only if it was last written before the task started;
    // one written during the run may not be what the tool read, and the trace
    // then keeps the pre-run digest (the next build reruns the task).
    Hash256 stored_digest = *digest;
    if (collected_deps != dyn_deps) {
        bool trusted = true;
        for (auto& p : collected_deps)
            if (!hashed_before.count(p) && !written_before(workspace_root_ / p, started)) {
                trusted = false;
                break;
            }
        if (trusted) {
            auto after = compute_action_digest(
                decl, collected_deps,
                [&](const std::string& p) -> kiln::Result<Hash256> {
                    if (auto it = hashed_before.find(p); it != hashed_before.end())
                        return kiln::Result<Hash256>::success(it->second);
                    auto hv = hash_cache_.hash_of(workspace_root_, p);
                    if (hv.ok())
                        hashed_before[p] = *hv;
                    return hv;
                },
                dep_summaries);
            if (after.ok())
                stored_digest = *after;
        }
    }

    Trace new_trace;
    new_trace.task_hash = th;
    new_trace.task_display = decl.id.display();
    new_trace.action_digest = stored_digest;
    new_trace.outputs = outputs;
    new_trace.dynamic_deps = collected_deps;
    new_trace.params_json = params_to_json(decl.params);
    new_trace.rule_version = decl.rule_version;
    new_trace.config_hash = decl.id.config_hash;
    new_trace.wall_ms = static_cast<int64_t>(wall_ms);
    new_trace.when_ns = static_cast<int64_t>(util::now_ms() * 1000000);

    // Per-input hashes for why-dirty.
    std::set<std::string> all_inputs;
    for (auto& p : decl.declared_inputs)
        all_inputs.insert(p);
    for (auto& p : decl.optional_inputs) // an absent one is not recorded
        all_inputs.insert(p);
    for (auto& p : collected_deps)
        all_inputs.insert(p);
    for (auto& p : all_inputs) {
        if (auto it = hashed_before.find(p); it != hashed_before.end()) {
            new_trace.inputs.push_back({p, it->second});
            continue;
        }
        auto hv = hash_cache_.hash_of(workspace_root_, p);
        if (!hv.ok())
            continue;
        new_trace.inputs.push_back({p, *hv});
    }

    // A second `out.log = logbuf->take()` used to sit here: it
    // re-took the already-drained buffer and overwrote the task's real
    // log with an empty string - every successful task's diagnostics
    // (compiler warnings included) were silently dropped.

    bool changed =
        !old_trace ||
        output_summary_hash(old_trace->outputs) != output_summary_hash(new_trace.outputs);

    if (auto err = traces_.put(new_trace); !err.ok()) {
        out.state = TaskState::Failed;
        out.why = err.error;
        out.why_ref = Result::WhyRef{.kind = "error", .msg = out.why};
        return out;
    }

    // Why.
    if (missing_output) {
        out.why = "output missing or modified on disk: " + missing_path;
        out.why_ref = Result::WhyRef{.kind = "missing_output", .path = missing_path,
                                     .msg = out.why};
    } else if (!old_trace) {
        out.why = "first build (no previous trace)";
        out.why_ref = Result::WhyRef{.kind = "first_build"};
    } else if (old_trace->config_hash != new_trace.config_hash) {
        out.why = "configuration changed";
        out.why_ref = Result::WhyRef{.kind = "config_changed"};
    } else if (old_trace->rule_version != decl.rule_version) {
        out.why = "rule version changed (" + std::to_string(old_trace->rule_version) +
                  " -> " + std::to_string(decl.rule_version) + ")";
        out.why_ref = Result::WhyRef{.kind = "rule_version",
                       .was = std::to_string(old_trace->rule_version),
                       .now = std::to_string(decl.rule_version)};
    } else if (old_trace->params_json != new_trace.params_json) {
        out.why = "params changed";
        out.why_ref = Result::WhyRef{.kind = "params_changed"};
    } else {
        // Input diffs.
        std::map<std::string, Hash256> old_in;
        for (auto& e : old_trace->inputs)
            old_in[e.path] = e.hash;
        bool found = false;
        for (auto& e : new_trace.inputs) {
            auto it = old_in.find(e.path);
            if (it == old_in.end()) {
                out.why = "new input appeared: " + e.path;
                out.why_ref = Result::WhyRef{.kind = "input_changed", .path = e.path, .was = "<none>",
                               .now = e.hash.hex()};
                found = true;
                break;
            }
            if (it->second != e.hash) {
                out.why = "input changed: " + e.path;
                out.why_ref = Result::WhyRef{.kind = "input_changed", .path = e.path,
                               .was = it->second.hex(), .now = e.hash.hex()};
                found = true;
                break;
            }
        }
        if (!found) {
            // Dep outputs changed this session.
            for (auto& d : dep_info) {
                if (d.outputs_changed) {
                    out.why = "dependency rebuilt with different outputs: " + d.id.display();
                    out.why_ref = Result::WhyRef{.kind = "dep_changed", .dep_display = d.id.display()};
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            // Dynamic dep set changed.
            out.why = "discovered dependency set changed";
            out.why_ref = Result::WhyRef{.kind = "params_changed",
                           .msg = "dynamic dependency set changed"};
        }
    }

    out.state = TaskState::Succeeded;
    out.trace = std::move(new_trace);
    out.outputs_changed = changed;
    return out;
}

} // namespace kiln

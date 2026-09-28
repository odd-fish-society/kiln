#include "cli/cli_common.h"

#include <algorithm>
#include <functional>
#include <set>

#include "core/util.h"
#include "nlohmann/json.hpp"
#include "workspace/workspace.h"

namespace kiln {

using json = nlohmann::json;

namespace {

// Reads graph.json for the given config (or the most recently modified one).
Result<json> load_graph(const fs::path& root, const std::string& config) {
    fs::path graph_dir = root / ".kiln/graph";
    std::error_code ec;
    if (!fs::is_directory(graph_dir, ec))
        return Result<json>::fail("no built graph found — run `kiln build` first");
    std::string chosen = config;
    if (chosen.empty()) {
        // Pick the most recently modified config dir.
        fs::file_time_type newest{};
        for (auto& e : fs::directory_iterator(graph_dir, ec)) {
            if (e.is_directory(ec)) {
                auto t = e.last_write_time(ec);
                if (chosen.empty() || t > newest) {
                    chosen = e.path().filename().generic_string();
                    newest = t;
                }
            }
        }
    }
    if (chosen.empty())
        return Result<json>::fail("no graphs found under " + graph_dir.generic_string());
    auto content = util::read_file(graph_dir / chosen / "graph.json");
    if (!content.ok())
        return Result<json>::fail(content.error);
    try {
        return Result<json>::success(json::parse(*content));
    } catch (...) {
        return Result<json>::fail("corrupt graph.json for " + chosen);
    }
}

} // namespace

Result<int> cmd_query(const fs::path& root_in, const std::string& expr,
                      const std::string& config, bool json_out) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }
    auto g = load_graph(root, config);
    if (!g.ok())
        return Result<int>::fail(g.error);

    // Collect nodes.
    struct Node {
        std::string display, rule, primary;
        std::vector<std::string> deps;
    };
    std::vector<Node> nodes;
    std::map<std::string, size_t> by_display;
    for (auto& n : g->at("nodes")) {
        Node node;
        node.display = n.at("display").get<std::string>();
        node.rule = n.value("rule", "");
        node.primary = n.value("primary_input", "");
        for (auto& d : n.value("deps", json::array()))
            node.deps.push_back(d.get<std::string>());
        by_display[node.display] = nodes.size();
        nodes.push_back(std::move(node));
    }

    std::set<std::string> results;

    auto parse_verb = [&](const std::string& e) -> std::pair<std::string, std::string> {
        auto open = e.find('(');
        if (open == std::string::npos)
            return {"prefix", e};
        auto close = e.rfind(')');
        if (close == std::string::npos || close < open)
            return {"prefix", e};
        return {e.substr(0, open), e.substr(open + 1, close - open - 1)};
    };

    auto [verb, arg] = parse_verb(expr);

    auto matches = [&](const Node& n, const std::string& a) {
        return n.display.find(a) != std::string::npos ||
               n.primary.find(a) != std::string::npos ||
               n.rule.find(a) != std::string::npos;
    };

    if (verb == "all") {
        for (auto& n : nodes)
            results.insert(n.display);
    } else if (verb == "deps") {
        for (auto& n : nodes)
            if (matches(n, arg))
                for (auto& d : n.deps)
                    results.insert(d);
    } else if (verb == "rdeps") {
        std::set<std::string> seeds;
        for (auto& n : nodes)
            if (matches(n, arg))
                seeds.insert(n.display);
        // Transitive reverse closure.
        std::set<std::string> visited;
        std::vector<std::string> stack(seeds.begin(), seeds.end());
        while (!stack.empty()) {
            std::string cur = stack.back();
            stack.pop_back();
            if (visited.count(cur))
                continue;
            visited.insert(cur);
            for (auto& n : nodes)
                for (auto& d : n.deps)
                    if (d == cur && !visited.count(n.display))
                        stack.push_back(n.display);
        }
        for (auto& v : visited)
            if (!seeds.count(v))
                results.insert(v);
    } else { // prefix filter
        for (auto& n : nodes)
            if (matches(n, expr))
                results.insert(n.display);
    }

    if (results.empty())
        return Result<int>::success(0);

    if (json_out) {
        json arr = json::array();
        for (auto& r : results)
            arr.push_back(r);
        std::printf("%s\n", arr.dump(2).c_str());
        return Result<int>::success(0);
    }
    for (auto& r : results)
        std::printf("%s\n", r.c_str());
    return Result<int>::success(0);
}

Result<int> cmd_why_dirty(const fs::path& root_in, const std::string& arg) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }
    auto content = util::read_file(root / ".kiln/logs/last_why.json");
    if (!content.ok())
        return Result<int>::fail("no build log — run `kiln build` first");
    json doc;
    try {
        doc = json::parse(*content);
    } catch (...) {
        return Result<int>::fail("corrupt " + (root / ".kiln/logs/last_why.json").generic_string());
    }

    struct Entry {
        std::string task, state, kind, path, was, now, dep, msg;
    };
    std::vector<Entry> entries;
    for (auto& e : doc.at("tasks")) {
        Entry en;
        en.task = e.at("task").get<std::string>();
        en.state = e.value("state", "");
        en.kind = e.value("kind", "");
        en.path = e.value("path", "");
        en.was = e.value("was", "");
        en.now = e.value("now", "");
        en.dep = e.value("dep", "");
        en.msg = e.value("msg", "");
        entries.push_back(std::move(en));
    }

    const Entry* start = nullptr;
    for (auto& e : entries) {
        if (e.task == arg || e.task.find(arg) != std::string::npos || e.path == arg) {
            start = &e;
            break;
        }
    }
    if (!start)
        return Result<int>::fail("task '" + arg + "' not found in the last build log");

    auto find_entry = [&](const std::string& display) -> const Entry* {
        for (auto& e : entries)
            if (e.task == display)
                return &e;
        return nullptr;
    };

    std::function<void(const Entry&, int)> print_entry = [&](const Entry& e, int depth) {
        std::string indent(depth * 2, ' ');
        if (depth == 0) {
            std::string verb = e.state == "cached" ? "UP TO DATE" : "REBUILT";
            std::printf("%s  %s", e.task.c_str(), verb.c_str());
            if (verb == "UP TO DATE") {
                std::printf("\n");
                return;
            }
            std::printf(" because:\n");
        } else {
            std::printf("%s", indent.c_str());
        }
        std::string prefix = depth == 0 ? "  " : indent + "because ";
        if (e.kind == "dep_changed") {
            std::printf("%sdep %s changed\n", prefix.c_str(), e.dep.c_str());
            if (auto* dep = find_entry(e.dep))
                print_entry(*dep, depth + 1);
        } else if (e.kind == "upstream_failed") {
            std::printf("%supstream failed: %s\n", prefix.c_str(), e.dep.c_str());
            if (auto* dep = find_entry(e.dep))
                print_entry(*dep, depth + 1);
        } else if (e.kind == "input_changed") {
            auto shorten = [](const std::string& h) {
                return h.size() > 8 ? h.substr(0, 8) + "…" : h;
            };
            std::printf("%s%s content changed (was %s, now %s)\n", prefix.c_str(),
                        e.path.c_str(), shorten(e.was).c_str(), shorten(e.now).c_str());
        } else if (e.kind == "missing_output") {
            std::printf("%soutput missing or modified: %s\n", prefix.c_str(),
                        e.path.c_str());
        } else if (e.kind == "params_changed") {
            std::printf("%sparams changed\n", prefix.c_str());
        } else if (e.kind == "config_changed") {
            std::printf("%sconfiguration changed\n", prefix.c_str());
        } else if (e.kind == "rule_version") {
            std::printf("%srule version changed (was %s, now %s)\n", prefix.c_str(),
                        e.was.c_str(), e.now.c_str());
        } else if (e.kind == "first_build") {
            std::printf("%sfirst build (no previous trace)\n", prefix.c_str());
        } else if (e.kind == "error") {
            std::printf("%serror: %s\n", prefix.c_str(), e.msg.c_str());
        } else {
            std::printf("%s%s\n", prefix.c_str(), e.msg.c_str());
        }
    };
    print_entry(*start, 0);
    return Result<int>::success(0);
}

Result<int> cmd_setup_clion(const fs::path& root_in, const std::string& config) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }
    std::string cfg = config;
    if (cfg.empty()) {
        fs::path dir = root / ".kiln/compile_commands";
        std::error_code ec;
        fs::file_time_type newest{};
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_directory(ec)) {
                auto t = e.last_write_time(ec);
                if (cfg.empty() || t > newest) {
                    cfg = e.path().filename().generic_string();
                    newest = t;
                }
            }
        }
    }
    if (cfg.empty())
        return Result<int>::fail(
            "no compile_commands generated — run `kiln build` first");
    fs::path src = root / ".kiln/compile_commands" / cfg / "compile_commands.json";
    if (!fs::is_regular_file(src))
        return Result<int>::fail("missing " + src.generic_string());
    fs::path dst = root / "compile_commands.json";
    std::error_code ec;
    fs::remove(dst, ec);
    fs::create_symlink(src, dst, ec);
    if (ec)
        return Result<int>::fail("cannot symlink: " + ec.message());
    std::printf("symlinked %s -> compile_commands.json\n", src.generic_string().c_str());
    std::printf("CLion: File > Open → pick the workspace root (it is a compilation database project).\n");
    std::printf("Run configurations: use `kiln build` / `kiln daemon` as external tools.\n");
    return Result<int>::success(0);
}

Result<int> cmd_clean(const fs::path& root_in, bool traces) {
    fs::path root = root_in;
    if (root.empty()) {
        auto found = find_workspace_root(fs::current_path());
        if (!found.ok())
            return Result<int>::fail(found.error);
        root = *found;
    }
    std::error_code ec;
    fs::remove_all(root / ".kiln/out", ec);
    fs::remove_all(root / ".kiln/graph", ec);
    fs::remove_all(root / ".kiln/compile_commands", ec);
    if (traces) {
        fs::remove_all(root / ".kiln/traces.sqlite", ec);
        fs::remove_all(root / ".kiln/traces.sqlite-wal", ec);
        fs::remove_all(root / ".kiln/traces.sqlite-shm", ec);
    }
    std::printf("cleaned .kiln/out, .kiln/graph, .kiln/compile_commands%s\n",
                traces ? ", and the trace store" : "");
    return Result<int>::success(0);
}

} // namespace kiln

// K6 tests (spec §8): the four-layer resolution, table-driven.
#include "doctest.h"

#include <map>

#include "workspace/resolve.h"
#include "test_util.h"

using namespace kiln;

namespace {

luau::PluginDecl decl(const std::string& name, bool enabled_by_default = true,
                      std::vector<std::string> platforms = {},
                      std::vector<std::string> configs = {},
                      std::vector<std::string> pub = {}, std::vector<std::string> priv = {},
                      std::vector<std::string> opt = {}) {
    luau::PluginDecl d;
    d.name = name;
    d.enabled_by_default = enabled_by_default;
    d.supported_platforms = std::move(platforms);
    d.supported_configs = std::move(configs);
    d.public_deps = std::move(pub);
    d.private_deps = std::move(priv);
    d.optional_deps = std::move(opt);
    d.file = "plugins/" + name + "/plugin.build.luau";
    d.line = 1;
    return d;
}

std::map<std::string, luau::PluginDecl> m(std::vector<luau::PluginDecl> v) {
    std::map<std::string, luau::PluginDecl> out;
    for (auto& d : v)
        out[d.name] = std::move(d);
    return out;
}

std::optional<luau::TargetDecl> target(std::string name,
                                       std::vector<std::string> enable = {},
                                       std::vector<std::string> disable = {}) {
    luau::TargetDecl t;
    t.name = std::move(name);
    t.platforms = {"linux"};
    t.enable_plugins = std::move(enable);
    t.disable_plugins = std::move(disable);
    t.file = "build/game.target.luau";
    t.line = 1;
    return t;
}

std::map<std::string, bool> states_of(const ResolvedPlugins& r) {
    std::map<std::string, bool> out;
    for (auto& s : r.states)
        out[s.name] = s.enabled;
    return out;
}

} // namespace

TEST_CASE("K6.1 default layer") {
    auto r = resolve_enabled(m({decl("a"), decl("b", false)}), target("t"), "linux", "dev",
                             {}, {});
    REQUIRE(r.ok());
    auto st = states_of(*r);
    CHECK(st["a"]);
    CHECK(!st["b"]);
}

TEST_CASE("K6.2 platform/config support is final") {
    auto r = resolve_enabled(m({decl("win_only", true, {"windows"})}), target("t"), "linux",
                             "dev", {}, {});
    REQUIRE(r.ok());
    CHECK(!states_of(*r)["win_only"]);

    // Attempting to enable via target is an ERROR naming the constraint.
    auto r2 = resolve_enabled(m({decl("win_only", true, {"windows"})}),
                              target("t", {"win_only"}), "linux", "dev", {}, {});
    REQUIRE(!r2.ok());
    CHECK(r2.error.find("win_only") != std::string::npos);

    auto r3 = resolve_enabled(m({decl("dev_only", true, {}, {"dev"})}), target("t"),
                              "linux", "release", {}, {});
    REQUIRE(r3.ok());
    CHECK(!states_of(*r3)["dev_only"]);
}

TEST_CASE("K6.3 target layer overrides default") {
    auto r = resolve_enabled(m({decl("a"), decl("b", false)}),
                             target("t", {"b"}, {"a"}), "linux", "dev", {}, {});
    REQUIRE(r.ok());
    auto st = states_of(*r);
    CHECK(!st["a"]);
    CHECK(st["b"]);
}

TEST_CASE("K6.4 CLI layer wins over target") {
    auto r = resolve_enabled(m({decl("a", true)}), target("t", {}, {"a"}), "linux", "dev",
                             {"a"}, {});
    REQUIRE(r.ok());
    CHECK(states_of(*r)["a"]);

    auto r2 = resolve_enabled(m({decl("a", true)}), target("t", {"a"}), "linux", "dev",
                              {}, {"a"});
    REQUIRE(r2.ok());
    CHECK(!states_of(*r2)["a"]);
}

TEST_CASE("K6.5 closure: enabled plugin with disabled non-optional dep errors") {
    auto r = resolve_enabled(m({decl("game", true, {}, {}, {"audio"}),
                                decl("audio", false)}),
                             target("t"), "linux", "dev", {}, {});
    REQUIRE(r.ok()); // resolution itself succeeds
    auto closure = check_dep_closure(
        m({decl("game", true, {}, {}, {"audio"}), decl("audio", false)}), *r, target("t"));
    REQUIRE(!closure.ok());
    CHECK(closure.error.find("game") != std::string::npos);
    CHECK(closure.error.find("audio") != std::string::npos);
    CHECK(closure.error.find("optional") != std::string::npos);
}

TEST_CASE("K6.6 optional deps: recorded missing when disabled") {
    auto r = resolve_enabled(m({decl("game", true, {}, {}, {}, {}, {"profiler"}),
                                decl("profiler", false)}),
                             target("t"), "linux", "dev", {}, {});
    REQUIRE(r.ok());
    const PluginState* game = nullptr;
    for (auto& s : r->states)
        if (s.name == "game")
            game = &s;
    REQUIRE(game);
    REQUIRE(game->optional_missing.size() == 1);
    CHECK(game->optional_missing[0] == "profiler");
}

TEST_CASE("K6.7 cycle detection with path in message") {
    auto plugins = m({decl("a", true, {}, {}, {"b"}), decl("b", true, {}, {}, {"a"})});
    auto r = resolve_enabled(plugins, target("t"), "linux", "dev", {}, {});
    REQUIRE(r.ok());
    auto cyc = check_plugin_cycles(plugins, r->enabled);
    REQUIRE(!cyc.ok());
    CHECK(cyc.error.find("cycle") != std::string::npos);
    CHECK(cyc.error.find("a") != std::string::npos);
    CHECK(cyc.error.find("b") != std::string::npos);
}

TEST_CASE("K6.8 enabled list is sorted") {
    auto r = resolve_enabled(m({decl("z"), decl("a")}), target("t"), "linux", "dev", {}, {});
    REQUIRE(r.ok());
    std::vector<std::string> ez{"a", "z"};
    REQUIRE(r->enabled == ez);
}

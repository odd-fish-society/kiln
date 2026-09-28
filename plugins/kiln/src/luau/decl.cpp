#include "luau/decl.h"

#include <algorithm>

#include "nlohmann/json.hpp"

namespace kiln::luau {

using json = nlohmann::json;

namespace {
json strvec(const std::vector<std::string>& v) {
    json j = json::array();
    for (auto& s : v)
        j.push_back(s);
    return j;
}
std::vector<std::string> to_strvec(const json& j) {
    std::vector<std::string> out;
    if (j.is_array())
        for (auto& s : j)
            out.push_back(s.get<std::string>());
    return out;
}
} // namespace

std::string decl_set_to_json(const DeclSet& decls) {
    json j;
    j["schema"] = "declset";
    j["version"] = 1;
    if (decls.plugin) {
        auto& p = *decls.plugin;
        j["plugin"] = {{"name", p.name},
                       {"enabled_by_default", p.enabled_by_default},
                       {"supported_platforms", strvec(p.supported_platforms)},
                       {"supported_configs", strvec(p.supported_configs)},
                       {"public_deps", strvec(p.public_deps)},
                       {"private_deps", strvec(p.private_deps)},
                       {"optional_deps", strvec(p.optional_deps)},
                       {"version", p.version},
                       {"file", p.file},
                       {"line", p.line}};
    }
    auto mods = json::array();
    for (auto& m : decls.modules) {
        mods.push_back({{"plugin", m.plugin},
                        {"name", m.name},
                        {"sources", strvec(m.sources)},
                        {"public_includes", strvec(m.public_includes)},
                        {"private_includes", strvec(m.private_includes)},
                        {"public_defines", strvec(m.public_defines)},
                        {"private_defines", strvec(m.private_defines)},
                        {"deps_public", strvec(m.deps_public)},
                        {"deps_private", strvec(m.deps_private)},
                        {"platforms", strvec(m.platforms)},
                        {"configs", strvec(m.configs)},
                        {"cpp_std", m.cpp_std},
                        {"pch", m.pch},
                        {"syslibs", strvec(m.syslibs)},
                        {"public_syslibs", strvec(m.public_syslibs)},
                        {"public_link_flags", strvec(m.public_link_flags)},
                        {"flags", strvec(m.flags)},
                        {"file", m.file},
                        {"line", m.line}});
    }
    j["modules"] = mods;
    auto cooks = json::array();
    for (auto& c : decls.cooks) {
        json params = json::object();
        for (auto& [k, v] : c.params)
            params[k] = v;
        cooks.push_back({{"plugin", c.plugin},
                         {"name", c.name},
                         {"rule", c.rule},
                         {"inputs", strvec(c.inputs)},
                         {"params", params},
                         {"platforms", strvec(c.platforms)},
                         {"configs", strvec(c.configs)},
                         {"file", c.file},
                         {"line", c.line}});
    }
    j["cooks"] = cooks;
    auto clibs = json::array();
    for (auto& c : decls.c_libraries) {
        clibs.push_back({{"plugin", c.plugin},
                         {"name", c.name},
                         {"sources", strvec(c.sources)},
                         {"includes", strvec(c.includes)},
                         {"public_includes", strvec(c.public_includes)},
                         {"defines", strvec(c.defines)},
                         {"platforms", strvec(c.platforms)},
                         {"configs", strvec(c.configs)},
                         {"c_std", c.c_std},
                         {"cpp_std", c.cpp_std},
                         {"syslibs", strvec(c.syslibs)},
                         {"public_syslibs", strvec(c.public_syslibs)},
                         {"public_link_flags", strvec(c.public_link_flags)},
                         {"flags", strvec(c.flags)},
                         {"file", c.file},
                         {"line", c.line}});
    }
    j["c_libraries"] = clibs;
    if (decls.target) {
        auto& t = *decls.target;
        json features = json::object();
        for (auto& [k, v] : t.features)
            features[k] = v;
        j["target"] = {{"name", t.name},
                       {"platforms", strvec(t.platforms)},
                       {"default_config", t.default_config},
                       {"enable_plugins", strvec(t.enable_plugins)},
                       {"disable_plugins", strvec(t.disable_plugins)},
                       {"features", features},
                       {"link", strvec(t.link)},
                       {"syslibs", strvec(t.syslibs)},
                       {"link_flags", strvec(t.link_flags)},
                       {"file", t.file},
                       {"line", t.line}};
    }
    if (decls.workspace) {
        auto& w = *decls.workspace;
        json pools = json::object();
        for (auto& [k, v] : w.pools)
            pools[k] = v;
        json tools = json::object();
        for (auto& [k, v] : w.tools)
            tools[k] = v;
        json roots = json::array();
        for (auto& pr : w.plugin_roots)
            roots.push_back({{"path", pr.path}, {"layer", pr.layer}});
        j["workspace"] = {{"name", w.name},
                          {"pools", pools},
                          {"jobs", w.jobs},
                          {"tools", tools},
                          {"strict_globs", w.strict_globs},
                          {"cpp_std", w.cpp_std},
                          {"c_std", w.c_std},
                          {"env_allow", strvec(w.env_allow)},
                          {"default_config", w.default_config},
                          {"system_libs", strvec(w.system_libs)},
                          {"plugin_roots_declared", w.plugin_roots_declared},
                          {"plugin_roots", roots}};
    }
    j["required_files"] = strvec(decls.required_files);
    auto globs = json::array();
    for (auto& g : decls.globs)
        globs.push_back({{"pattern", g.pattern}, {"files", strvec(g.files)}});
    j["globs"] = globs;
    return j.dump(2);
}



bool decl_set_from_json(const std::string& s, DeclSet& out) {
    try {
        json j = json::parse(s);
        if (j.contains("plugin")) {
            PluginDecl p;
            auto& v = j["plugin"];
            p.name = v.at("name").get<std::string>();
            p.enabled_by_default = v.value("enabled_by_default", true);
            p.supported_platforms = to_strvec(v.value("supported_platforms", json::array()));
            p.supported_configs = to_strvec(v.value("supported_configs", json::array()));
            p.public_deps = to_strvec(v.value("public_deps", json::array()));
            p.private_deps = to_strvec(v.value("private_deps", json::array()));
            p.optional_deps = to_strvec(v.value("optional_deps", json::array()));
            p.version = v.value("version", "");
            p.file = v.value("file", "");
            p.line = v.value("line", 0);
            out.plugin = std::move(p);
        }
        for (auto& v : j.value("modules", json::array())) {
            ModuleDecl m;
            m.plugin = v.at("plugin").get<std::string>();
            m.name = v.at("name").get<std::string>();
            m.sources = to_strvec(v.at("sources"));
            m.public_includes = to_strvec(v.value("public_includes", json::array()));
            m.private_includes = to_strvec(v.value("private_includes", json::array()));
            m.public_defines = to_strvec(v.value("public_defines", json::array()));
            m.private_defines = to_strvec(v.value("private_defines", json::array()));
            m.deps_public = to_strvec(v.value("deps_public", json::array()));
            m.deps_private = to_strvec(v.value("deps_private", json::array()));
            m.platforms = to_strvec(v.value("platforms", json::array()));
            m.configs = to_strvec(v.value("configs", json::array()));
            m.cpp_std = v.value("cpp_std", "");
            m.pch = v.value("pch", "");
            m.syslibs = to_strvec(v.value("syslibs", json::array()));
            m.public_syslibs = to_strvec(v.value("public_syslibs", json::array()));
            m.public_link_flags = to_strvec(v.value("public_link_flags", json::array()));
            m.flags = to_strvec(v.value("flags", json::array()));
            m.file = v.value("file", "");
            m.line = v.value("line", 0);
            out.modules.push_back(std::move(m));
        }
        for (auto& v : j.value("cooks", json::array())) {
            CookDecl c;
            c.plugin = v.at("plugin").get<std::string>();
            c.name = v.at("name").get<std::string>();
            c.rule = v.at("rule").get<std::string>();
            c.inputs = to_strvec(v.at("inputs"));
            const json& params = v.value("params", json::object());
            for (auto pit = params.begin(); pit != params.end(); ++pit)
                c.params[pit.key()] = pit.value().get<std::string>();
            c.platforms = to_strvec(v.value("platforms", json::array()));
            c.configs = to_strvec(v.value("configs", json::array()));
            c.file = v.value("file", "");
            c.line = v.value("line", 0);
            out.cooks.push_back(std::move(c));
        }
        for (auto& v : j.value("c_libraries", json::array())) {
            CLibraryDecl c;
            c.plugin = v.at("plugin").get<std::string>();
            c.name = v.at("name").get<std::string>();
            c.sources = to_strvec(v.at("sources"));
            c.includes = to_strvec(v.value("includes", json::array()));
            c.public_includes = to_strvec(v.value("public_includes", json::array()));
            c.defines = to_strvec(v.value("defines", json::array()));
            c.platforms = to_strvec(v.value("platforms", json::array()));
            c.configs = to_strvec(v.value("configs", json::array()));
            c.c_std = v.value("c_std", "");
            c.cpp_std = v.value("cpp_std", "");
            c.syslibs = to_strvec(v.value("syslibs", json::array()));
            c.public_syslibs = to_strvec(v.value("public_syslibs", json::array()));
            c.public_link_flags = to_strvec(v.value("public_link_flags", json::array()));
            c.flags = to_strvec(v.value("flags", json::array()));
            c.file = v.value("file", "");
            c.line = v.value("line", 0);
            out.c_libraries.push_back(std::move(c));
        }
        if (j.contains("target")) {
            TargetDecl t;
            auto& v = j["target"];
            t.name = v.at("name").get<std::string>();
            t.platforms = to_strvec(v.value("platforms", json::array()));
            t.default_config = v.value("default_config", "");
            t.enable_plugins = to_strvec(v.value("enable_plugins", json::array()));
            t.disable_plugins = to_strvec(v.value("disable_plugins", json::array()));
            const json& feats = v.value("features", json::object());
            for (auto fit = feats.begin(); fit != feats.end(); ++fit)
                t.features[fit.key()] = fit.value().get<std::string>();
            t.link = to_strvec(v.value("link", json::array()));
            t.syslibs = to_strvec(v.value("syslibs", json::array()));
            t.link_flags = to_strvec(v.value("link_flags", json::array()));
            t.file = v.value("file", "");
            t.line = v.value("line", 0);
            out.target = std::move(t);
        }
        if (j.contains("workspace")) {
            WorkspaceDecl w;
            auto& v = j["workspace"];
            w.name = v.value("name", "");
            const json& pools = v.value("pools", json::object());
            for (auto pit = pools.begin(); pit != pools.end(); ++pit)
                w.pools[pit.key()] = pit.value().get<int>();
            w.jobs = v.value("jobs", 0);
            const json& tools = v.value("tools", json::object());
            for (auto tit = tools.begin(); tit != tools.end(); ++tit)
                w.tools[tit.key()] = tit.value().get<std::string>();
            w.strict_globs = v.value("strict_globs", false);
            w.cpp_std = v.value("cpp_std", "c++20");
            w.c_std = v.value("c_std", "c11");
            w.env_allow = to_strvec(v.value("env_allow", json::array()));
            w.default_config = v.value("default_config", "dev");
            w.system_libs = to_strvec(v.value("system_libs", json::array()));
            w.plugin_roots_declared = v.value("plugin_roots_declared", false);
            for (auto& rv : v.value("plugin_roots", json::array())) {
                PluginRootDecl pr;
                pr.path = rv.value("path", "");
                pr.layer = rv.value("layer", "");
                w.plugin_roots.push_back(std::move(pr));
            }
            out.workspace = std::move(w);
        }
        out.required_files = to_strvec(j.value("required_files", json::array()));
        for (auto& v : j.value("globs", json::array())) {
            FileSet g;
            g.pattern = v.at("pattern").get<std::string>();
            g.files = to_strvec(v.at("files"));
            out.globs.push_back(std::move(g));
        }
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[decl] from_json error: %s\n", e.what());
        return false;
    } catch (...) {
        fprintf(stderr, "[decl] from_json error: unknown\n");
        return false;
    }
}

} // namespace kiln::luau

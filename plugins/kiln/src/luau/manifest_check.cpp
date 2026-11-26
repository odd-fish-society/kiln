#include "luau/manifest_check.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Config.h"
#include "Luau/Frontend.h"
#include "Luau/Parser.h"
#include "Luau/ToString.h"
#include "cluau/check.h"
#include "cluau/cluau.h"
#include "core/util.h"

namespace kiln::luau {

namespace {

namespace fs = std::filesystem;

struct ManifestConfig : ::Luau::ConfigResolver {
    ::Luau::Config nonstrict;
    ::Luau::Config strict;
    ManifestConfig() {
        nonstrict.mode = ::Luau::Mode::Nonstrict;
        strict.mode = ::Luau::Mode::Strict;
    }
    const ::Luau::Config& getConfig(const ::Luau::ModuleName& name) const override {
        return cluau::IsCluauPath(name) ? strict : nonstrict;
    }
};

struct ManifestFiles : ::Luau::FileResolver {
    fs::path root;
    std::map<std::string, cluau::TranslateResult> translations; // by module name (workspace-relative)
    std::set<std::string> libraries;                             // translated outside manifest mode

    std::optional<::Luau::SourceCode> readSource(const ::Luau::ModuleName& name) override {
        auto text = util::read_file(root / name);
        if (!text.ok())
            return std::nullopt;
        if (!cluau::IsCluauPath(name))
            return ::Luau::SourceCode{*text, ::Luau::SourceCode::Module};
        cluau::TranslateOptions options;
        options.Manifest = !libraries.count(name);
        cluau::TranslateResult& t = translations[name] = cluau::Translate(*text, options);
        return ::Luau::SourceCode{t.Luau, ::Luau::SourceCode::Module};
    }

    // Kiln's require form: `require "build/x"` is build/x.cluau or build/x.luau.
    std::optional<::Luau::ModuleInfo> resolveModule(const ::Luau::ModuleInfo*, ::Luau::AstExpr* node) override {
        auto* expr = node->as<::Luau::AstExprConstantString>();
        if (!expr)
            return std::nullopt;
        std::string rel = util::norm(std::string(expr->value.data, expr->value.size));
        if (rel.find('.') == std::string::npos) {
            std::error_code ec;
            rel += fs::is_regular_file(root / (rel + ".cluau"), ec) ? ".cluau" : ".luau";
        }
        std::error_code ec;
        if (!fs::is_regular_file(root / rel, ec))
            return std::nullopt;
        return ::Luau::ModuleInfo{rel};
    }

    std::string getHumanReadableModuleName(const ::Luau::ModuleName& name) const override { return name; }
};

} // namespace

Result<std::vector<CheckFinding>> check_manifests(const fs::path& workspace_root, const std::vector<std::string>& files,
                                                  const std::string& definitions,
                                                  const std::vector<std::string>& libraries) {
    ManifestFiles resolver;
    resolver.root = workspace_root;
    resolver.libraries.insert(libraries.begin(), libraries.end());
    ManifestConfig config;
    ::Luau::FrontendOptions options;
    options.retainFullTypeGraphs = true; // cluau_check reads the inferred types
    ::Luau::Frontend frontend(&resolver, &config, options);
    ::Luau::registerBuiltinGlobals(frontend, frontend.globals);
    ::Luau::LoadDefinitionFileResult loaded =
        frontend.loadDefinitionFile(frontend.globals, frontend.globals.globalScope, definitions, "@kiln", false, false);
    if (!loaded.success) {
        std::string why;
        for (auto& e : loaded.parseResult.errors)
            why += "\n  build.d.luau:" + std::to_string(e.getLocation().begin.line + 1) + ": " + e.getMessage();
        if (loaded.module)
            for (auto& e : loaded.module->errors)
                why += "\n  build.d.luau:" + std::to_string(e.location.begin.line + 1) + ": " + ::Luau::toString(e);
        return Result<std::vector<CheckFinding>>::fail("kiln check: the manifest definitions do not load:" + why);
    }
    ::Luau::freeze(frontend.globals.globalTypes);

    std::vector<CheckFinding> out;
    for (const std::string& file : files) {
        ::Luau::CheckResult result = frontend.check(file);
        auto tr = resolver.translations.find(file);
        const cluau::TranslateResult* translation = tr == resolver.translations.end() ? nullptr : &tr->second;
        auto at = [&](::Luau::Position p, CheckFinding& f) {
            cluau::Position pos{(int)p.line, (int)p.column};
            if (translation)
                pos = translation->Map.FromLuau(pos);
            f.line = pos.Line + 1;
            f.col = pos.Col + 1;
        };
        if (translation)
            for (const cluau::Diagnostic& d : translation->Diagnostics) {
                CheckFinding f{file, d.At.Begin.Line + 1, d.At.Begin.Col + 1, "cluau", d.Message, d.Warning};
                out.push_back(std::move(f));
            }
        for (const ::Luau::TypeError& e : result.errors) {
            if (e.moduleName != file)
                continue; // reported where it is (a required helper's own check)
            CheckFinding f;
            f.file = file;
            f.code = ::Luau::get<::Luau::SyntaxError>(e) ? "SyntaxError" : "TypeError";
            f.message = ::Luau::toString(e);
            at(e.location.begin, f);
            out.push_back(std::move(f));
        }
        if (translation) {
            const ::Luau::SourceModule* source = frontend.getSourceModule(file);
            ::Luau::ModulePtr module = frontend.moduleResolver.getModule(file);
            if (source && module)
                for (const cluau::check::Finding& c : cluau::check::Run(*source, *module, *translation)) {
                    CheckFinding f{file, c.At.Begin.Line + 1, c.At.Begin.Col + 1, c.Code, c.Message, c.Warning};
                    out.push_back(std::move(f));
                }
        }
    }
    std::sort(out.begin(), out.end(), [](const CheckFinding& a, const CheckFinding& b) {
        return a.file != b.file ? a.file < b.file : a.line != b.line ? a.line < b.line : a.col < b.col;
    });
    return Result<std::vector<CheckFinding>>::success(std::move(out));
}

} // namespace kiln::luau

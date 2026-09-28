#include "luau/ast_lint.h"

#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/Lexer.h"
#include "Luau/ParseResult.h"
#include "Luau/Parser.h"

namespace kiln::luau {

namespace {

struct ManifestLintVisitor : Luau::AstVisitor {
    std::vector<std::string> errors;

    bool visit(Luau::AstExprFunction* node) override {
        errors.push_back(std::to_string(node->location.begin.line) +
                         ": function definitions are not allowed in manifests");
        return true;
    }
    bool visit(Luau::AstStatFunction* node) override {
        errors.push_back(std::to_string(node->location.begin.line) +
                         ": function definitions are not allowed in manifests");
        return true;
    }
    bool visit(Luau::AstStatWhile* node) override {
        errors.push_back(std::to_string(node->location.begin.line) +
                         ": while loops are not allowed in manifests (use a numeric for)");
        return true;
    }
    bool visit(Luau::AstStatRepeat* node) override {
        errors.push_back(std::to_string(node->location.begin.line) +
                         ": repeat loops are not allowed in manifests (use a numeric for)");
        return true;
    }
};

} // namespace

Result<void> lint_manifest(const std::string& source, const std::string& file_name) {
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions options;
    Luau::ParseResult result;
    try {
        result = Luau::Parser::parse(source.data(), source.size(), names, allocator, options);
    } catch (const Luau::ParseErrors& e) {
        return Result<void>::fail(file_name + ": " + e.what());
    }
    if (!result.errors.empty()) {
        std::string msg = file_name + ": parse error: " + result.errors[0].getMessage();
        return Result<void>::fail(msg);
    }
    ManifestLintVisitor visitor;
    result.root->visit(&visitor);
    if (!visitor.errors.empty())
        return Result<void>::fail(file_name + ":" + visitor.errors[0]);
    return Result<void>::success();
}

} // namespace kiln::luau

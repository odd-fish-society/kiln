// Boundary lint (spec §14). Nothing under kiln/plugins
// may reference the consuming engine — no quoted #include
// escaping kiln/ + third_party/ + system paths, and no engine-identifying
// strings. This replaces the physical enforcement a separate repository
// would have provided; grep-level detection is deliberate.
#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "core/util.h"

namespace fs = std::filesystem;

namespace {

fs::path repo_root() { return fs::path(KILN_SOURCE_DIR); }

// Engine-identifying denylist (spec §14): the engine name plus the engine's
// configured plugin-root paths. A consuming-workspace layout change (e.g.
// moving plugins between roots) updates the denylist in the SAME commit.
const std::vector<std::string>& deny_strings() {
    static const std::vector<std::string> v = {
        "TinyEngine", "TinyEditor", "tiny-engine", "tiny_engine",
        // The engine workspace's configured plugin roots (M1 layout).
        "third_party/plugins", "engine/plugins", "editor/plugins", "game/plugins",
    };
    return v;
}

std::vector<fs::path> scan_roots() {
    return {repo_root() / "plugins", repo_root() / "skills"};
}

bool scannable(const fs::path& p) {
    auto ext = p.extension().generic_string();
    return ext == ".cpp" || ext == ".h" || ext == ".md" || ext == ".sh" ||
           ext == ".luau" || ext == ".c";
}

// Returns a violation description for `file`, or nullopt when clean.
std::optional<std::string> check_file(const fs::path& file) {
    auto content = kiln::util::read_file(file);
    if (!content.ok())
        return std::nullopt;

    std::istringstream in(*content);
    std::string line;
    size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        // Engine-identifying strings.
        for (auto& deny : deny_strings()) {
            if (line.find(deny) != std::string::npos) {
                return "engine-identifying string '" + deny + "' in " +
                       file.generic_string() + ":" + std::to_string(lineno);
            }
        }
        // Quoted includes that escape kiln/ (absolute or resolving outside
        // the repo root). Vendored trees legitimately use internal relative
        // includes ("../include/x.h") — those resolve INSIDE kiln/ and are
        // fine; only escapes past the repo root are boundary erosion.
        size_t pos = 0;
        while ((pos = line.find("#include \"", pos)) != std::string::npos) {
            size_t start = pos + 10;
            size_t end = line.find('"', start);
            if (end == std::string::npos)
                break;
            std::string inc = line.substr(start, end - start);
            if (!inc.empty() && inc[0] == '/') {
                return "absolute quoted include '" + inc + "' in " +
                       file.generic_string() + ":" + std::to_string(lineno);
            }
            if (inc.rfind("../", 0) == 0 || inc.find("/../") != std::string::npos) {
                fs::path resolved =
                    (file.parent_path() / inc).lexically_normal();
                std::string rs = resolved.generic_string();
                std::string rr = repo_root().generic_string();
                if (rs != rr && rs.rfind(rr + "/", 0) != 0) {
                    return "escaping quoted include '" + inc + "' in " +
                           file.generic_string() + ":" + std::to_string(lineno);
                }
            }
            pos = end + 1;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("boundary lint: no engine references under plugins/skills") {
    size_t scanned = 0;
    for (auto& root : scan_roots()) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (it->is_regular_file(ec) && scannable(it->path())) {
                // Skip self-cleaning violation fixtures from the companion test,
                // and this detector file itself (it embeds the denylist).
                std::string fn = it->path().filename().generic_string();
                if (fn.rfind(".boundary_", 0) == 0 || fn == "boundary_lint.cpp")
                    continue;
                auto violation = check_file(it->path());
                std::string vmsg = violation.value_or("");
                INFO(vmsg);
                REQUIRE_FALSE(violation.has_value());
                ++scanned;
            }
        }
    }
    CHECK(scanned > 50); // the tree must actually be scanned
}

TEST_CASE("boundary lint: a deliberate violation is detected") {
    // Two temporary fixtures: one whose quoted include resolves OUTSIDE
    // kiln/ (the lexical escape branch, no engine string involved), and one
    // naming the engine (the denylist branch).
    fs::path escape = repo_root() / "plugins/kiln_tests" / ".boundary_escape_tmp.cpp";
    std::ofstream f1(escape);
    f1 << "#include \"../../../../../outside-the-repo/x.h\"\n"
          "int escape_touch() { return 0; }\n";
    f1.close();
    auto v1 = check_file(escape);
    REQUIRE(v1.has_value());
    CHECK(v1->find("escaping quoted include") != std::string::npos);

    fs::path engine = repo_root() / "plugins/kiln_tests" / ".boundary_engine_tmp.cpp";
    std::ofstream f2(engine);
    f2 << "int engine_touch() { return TinyEngine::kEngineMagic; }\n";
    f2.close();
    auto v2 = check_file(engine);
    REQUIRE(v2.has_value());
    CHECK(v2->find("TinyEngine") != std::string::npos);

    std::error_code ec;
    fs::remove(escape, ec);
    fs::remove(engine, ec);
}

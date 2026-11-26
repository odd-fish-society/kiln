#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/hash.h"
#include "core/util.h"
#include "hamr/test.h"

namespace fs = std::filesystem;

namespace kiln::testutil {

// A self-destructing temp dir.
struct TempDir {
    fs::path path;
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Writes a tree: map rel path → content. Creates parent dirs.
void write_tree(const fs::path& root, const std::map<std::string, std::string>& files);

// Runs the kiln binary with args in dir; captures stdout+stderr and exit code.
// `env` overrides/adds environment variables for the child.
struct RunResult {
    int code = -1;
    std::string out;
};
RunResult run_kiln(const fs::path& dir, const std::vector<std::string>& args,
                   const std::map<std::string, std::string>& env = {});

// Normalizes output for golden comparisons: strips durations like "412 ms"
// and counters that vary.
std::string normalize(const std::string& s);

// Counts "[n/n] ok" lines (executed tasks) in plain-mode build output.
int count_executed(const std::string& out);
int count_cached(const std::string& out);
// A plugin graph file (`content.provider.json`, ...) of one platform-config:
// it lives in the configuration variant's folder,
// .kiln/graph/<platform-config>/<variant>/ (the single match; empty if none).
fs::path graph_file(const fs::path& root, const std::string& platform_config, const std::string& file);
// A code output under its variant folder: <root>/<dir>/<variant>/<rest>
// (the single match; empty if none). Objects, PCHs and archives live in one:
// `.kiln/out/<cfg>/obj/<plugin>/<module>/<variant>/...`.
fs::path variant_file(const fs::path& root, const std::string& dir, const std::string& rest);
int count_failed(const std::string& out);

// Minimal fixture workspace: core + app plugins, one target, one shader cook.
std::map<std::string, std::string> basic_fixture_files();

// A fake `glslc` shell script (concatenates inputs) for tool-fixture tests.
std::string fake_glslc_script();

std::string read_file(const fs::path& p);
bool file_exists(const fs::path& p);

} // namespace kiln::testutil

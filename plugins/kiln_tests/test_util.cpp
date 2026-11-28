#include "test_util.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kiln::testutil {

namespace {

fs::path self_dir() {
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
}

} // namespace

const std::string& kiln_binary() {
    static const std::string bin = (self_dir() / "kiln").string();
    return bin;
}

const fs::path& source_root() {
    static const fs::path root = [] {
        std::error_code ec;
        for (fs::path d = self_dir(); !d.empty(); d = d.parent_path()) {
            if (fs::exists(d / "kiln.workspace.cluau", ec))
                return d;
            if (d == d.root_path())
                break;
        }
        return fs::path();
    }();
    return root;
}

TempDir::TempDir() {
    char tmpl[] = "/tmp/kiln_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    path = dir;
}

TempDir::~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
}

void write_tree(const fs::path& root, const std::map<std::string, std::string>& files) {
    for (auto& [rel, content] : files) {
        fs::path p = root / rel;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << content;
    }
}

RunResult run_kiln(const fs::path& dir, const std::vector<std::string>& args,
                   const std::map<std::string, std::string>& env) {
    RunResult r;
    std::vector<std::string> argv = {kiln_binary()};
    for (auto& a : args)
        argv.push_back(a);

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return r;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        chdir(dir.c_str());
        for (auto& [k, v] : env)
            setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> cargv;
        for (auto& a : argv)
            cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execv(cargv[0], cargv.data());
        _exit(127);
    }
    close(pipefd[1]);
    char buf[16384];
    ssize_t n;
    std::string out;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<size_t>(n));
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    r.out = std::move(out);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

std::string normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        // Strip "NNN ms", "N.N s", "NmNNs" tokens.
        bool stripped = false;
        for (const char* unit : {" ms", " s"}) {
            if (s.compare(i, strlen(unit), unit) == 0) {
                // Walk back over the number.
                size_t j = i;
                while (j > 0 && (isdigit(s[j - 1]) || s[j - 1] == '.'))
                    --j;
                if (j < i) {
                    out.erase(out.size() - (i - j));
                    i += strlen(unit);
                    stripped = true;
                    break;
                }
            }
        }
        if (stripped)
            continue;
        if (s.compare(i, 2, "\x1b[") == 0) {
            // Skip ANSI escapes.
            while (i < s.size() && s[i] != 'm')
                ++i;
            if (i < s.size())
                ++i;
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

int count_executed(const std::string& out) {
    int n = 0;
    size_t pos = 0;
    while ((pos = out.find("] ok ", pos)) != std::string::npos) {
        ++n;
        ++pos;
    }
    return n;
}
int count_cached(const std::string& out) {
    int n = 0;
    size_t pos = 0;
    while ((pos = out.find("] cached ", pos)) != std::string::npos) {
        ++n;
        ++pos;
    }
    return n;
}
fs::path graph_file(const fs::path& root, const std::string& platform_config, const std::string& file) {
    std::error_code ec;
    for (auto& v : fs::directory_iterator(root / ".kiln/graph" / platform_config, ec))
        if (v.is_directory(ec) && fs::exists(v.path() / file, ec))
            return v.path() / file;
    return {};
}

fs::path variant_file(const fs::path& root, const std::string& dir, const std::string& rest) {
    std::error_code ec;
    for (auto& v : fs::directory_iterator(root / dir, ec))
        if (v.is_directory(ec) && fs::exists(v.path() / rest, ec))
            return v.path() / rest;
    return {};
}

int count_failed(const std::string& out) {
    int n = 0;
    size_t pos = 0;
    while ((pos = out.find("] failed ", pos)) != std::string::npos) {
        ++n;
        ++pos;
    }
    return n;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::map<std::string, std::string> basic_fixture_files() {
    return {
        {"kiln.workspace.cluau",
         "workspace \"demo\" {\n  pools = { link = 1 },\n  strict_globs = true,\n  "
         "cpp_std = \"c++20\",\n  default_config = \"dev\",\n}\n"},
        {"build/demo.target.cluau",
         "target \"demo\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
         " link = { \"app/main\" },\n}\n"},
        {"plugins/core/plugin.build.cluau",
         "plugin \"core\" {}\n\nmodule \"core\" {\n  sources = glob(\"src/*.cpp\"),\n  "
         "public_includes = { \"include\" },\n}\n"},
        {"plugins/core/include/core.h", "#pragma once\nint core_value();\n"},
        {"plugins/core/src/core.cpp",
         "#include \"core.h\"\nint core_value() { return 42; }\n"},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" { public_deps = { \"core\" } }\n\nmodule \"main\" {\n  sources = "
         "glob(\"src/*.cpp\"),\n  deps = { public = { \"core/core\" } },\n}\n"},
        {"plugins/app/src/main.cpp",
         "#include \"core.h\"\n#include <cstdio>\nint main() {\n  std::printf(\"%d\\n\", "
         "core_value());\n  return 0;\n}\n"},
    };
}

std::string fake_glslc_script() {
    return "#!/bin/sh\n"
           "input=\"\"\n"
           "out=\"\"\n"
           "prev=\"\"\n"
           "for arg in \"$@\"; do\n"
           "  case \"$arg\" in\n"
           "    -o) prev=o;;\n"
           "    -*) prev=\"\";;\n"
           "    *)\n"
           "      if [ \"$prev\" = o ]; then out=\"$arg\"; else input=\"$arg\"; fi\n"
           "      prev=\"\";;\n"
           "  esac\n"
           "done\n"
           "mkdir -p \"$(dirname \"$out\")\"\n"
           "echo \"// fake-compiled\" > \"$out\"\n"
           "echo \"// input: $input\" >> \"$out\"\n"
           "for arg in \"$@\"; do\n"
           "  case \"$arg\" in -D*) echo \"$arg\" >> \"$out\";; esac\n"
           "done\n"
           "exit 0\n";
}

} // namespace kiln::testutil

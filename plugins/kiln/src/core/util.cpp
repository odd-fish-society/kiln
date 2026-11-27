#include "core/util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kiln::util {

namespace {

constexpr size_t kMaxCapture = 4 * 1024 * 1024; // 4 MiB

std::string normalize(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool last_was_slash = false;
    size_t i = 0;
    for (; i < in.size(); ++i) {
        char c = in[i];
        if (c == '\\')
            c = '/';
        if (c == '/') {
            if (!last_was_slash)
                out.push_back('/');
            last_was_slash = true;
            continue;
        }
        if (c == '.' && last_was_slash && i + 1 < in.size() && in[i + 1] == '/') {
            ++i; // skip the '.'
            continue;
        }
        last_was_slash = false;
        out.push_back(c);
    }
    if (out.size() > 1 && out.back() == '/')
        out.pop_back();
    if (out.empty())
        out = ".";

    // Lexically resolve ".." components.
    std::vector<std::string> comps;
    std::string cur;
    bool absolute = out[0] == '/';
    size_t pos = absolute ? 1 : 0;
    while (pos <= out.size()) {
        size_t slash = out.find('/', pos);
        if (slash == std::string::npos)
            slash = out.size();
        cur = out.substr(pos, slash - pos);
        if (!cur.empty() && cur != ".") {
            if (cur == "..") {
                if (!comps.empty() && comps.back() != "..")
                    comps.pop_back();
                else if (!absolute)
                    comps.push_back(".."); // escape beyond root: preserved
            } else {
                comps.push_back(cur);
            }
        }
        if (slash == out.size())
            break;
        pos = slash + 1;
    }
    std::string result = absolute ? "/" : "";
    for (size_t k = 0; k < comps.size(); ++k) {
        if (k)
            result += "/";
        result += comps[k];
    }
    if (result.empty())
        result = ".";
    return result;
}

} // namespace

std::string norm(std::string_view p) { return normalize(p); }

Result<std::string> norm_rel(const fs::path& root, const fs::path& p) {
    std::string s = p.generic_string();
    if (!s.empty() && (s[0] == '/' || (s.size() > 1 && s[1] == ':')))
        return Result<std::string>::fail("absolute path not allowed: " + s);
    std::string n = normalize(s);
    // Reject ".." escapes.
    size_t pos = 0;
    std::vector<std::string> comps;
    std::string cur;
    while (pos < n.size()) {
        char c = n[pos++];
        if (c == '/') {
            if (!cur.empty()) {
                comps.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        comps.push_back(cur);
    int depth = 0;
    for (auto& comp : comps) {
        if (comp == "..") {
            if (--depth < 0)
                return Result<std::string>::fail("path escapes workspace root: " + s);
        } else if (comp != ".") {
            ++depth;
        }
    }
    (void)root;
    return Result<std::string>::success(n);
}

bool is_inside(const fs::path& root, const fs::path& p) {
    auto r = fs::weakly_canonical(root);
    auto q = fs::weakly_canonical(p);
    auto it = q.begin();
    for (auto rit = r.begin(); rit != r.end(); ++rit, ++it) {
        if (it == q.end() || *it != *rit)
            return false;
    }
    return true;
}

Result<std::string> read_file(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec))
        return Result<std::string>::fail("not a regular file: " + p.generic_string());
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return Result<std::string>::fail("cannot open for read: " + p.generic_string());
    std::ostringstream ss;
    ss << f.rdbuf();
    return Result<std::string>::success(ss.str());
}

Result<void> write_file(const fs::path& p, std::string_view content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f)
        return Result<void>::fail("cannot open for write: " + p.generic_string());
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    f.flush();
    if (!f)
        return Result<void>::fail("write failed: " + p.generic_string());
    return Result<void>::success();
}

Result<std::vector<fs::path>> list_dirs(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> out;
    if (!fs::is_directory(root, ec))
        return Result<std::vector<fs::path>>::fail("not a directory: " + root.generic_string());
    for (auto& e : fs::directory_iterator(root, ec)) {
        if (e.is_directory(ec))
            out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return Result<std::vector<fs::path>>::success(std::move(out));
}

Result<std::vector<std::string>> walk_files_rel(const fs::path& root_in) {
    // "a/b/" would make every lexically_relative "../x".
    const fs::path root = root_in.has_filename() ? root_in : root_in.parent_path();
    std::error_code ec;
    std::vector<std::string> out;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_symlink(ec))
            continue; // symlinks not followed
        if (it->is_regular_file(ec)) {
            // Lexical: the iterator's paths are `root / ...`, and
            // fs::relative canonicalizes both sides (a readlink per
            // component, per file).
            out.push_back(norm(it->path().lexically_relative(root).generic_string()));
        }
    }
    std::sort(out.begin(), out.end());
    return Result<std::vector<std::string>>::success(std::move(out));
}

std::string join_rel(std::string_view a, std::string_view b) {
    if (a.empty())
        return norm(b);
    if (b.empty())
        return norm(a);
    return norm(std::string(a) + "/" + std::string(b));
}

std::string path_join(std::string_view a, std::string_view b) { return join_rel(a, b); }

std::string path_parent(std::string_view p) {
    auto n = norm(p);
    auto pos = n.rfind('/');
    if (pos == std::string::npos)
        return "";
    if (pos == 0)
        return "/";
    return n.substr(0, pos);
}

std::string path_basename(std::string_view p) {
    auto n = norm(p);
    auto pos = n.rfind('/');
    if (pos == std::string::npos)
        return n;
    return n.substr(pos + 1);
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::string find_in_path(std::string_view name) {
    const char* path = std::getenv("PATH");
    if (!path)
        return {};
    std::string p = path;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos)
            end = p.size();
        std::string dir = p.substr(start, end - start);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + std::string(name);
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec)) {
                auto st = fs::status(candidate, ec);
                if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none)
                    return candidate;
            }
        }
        if (end == p.size())
            break;
        start = end + 1;
    }
    return {};
}

int run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                std::string* out) {
    return run_process(argv, cwd, out, {});
}

int run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                std::string* out, const std::map<std::string, std::string>& env) {
#ifdef __linux__
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        if (!cwd.empty())
            chdir(cwd.c_str());
        for (auto& [k, v] : env)
            setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (auto& a : argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(pipefd[1]);
    std::string buf;
    char chunk[16384];
    ssize_t n;
    size_t captured = 0;
    while ((n = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        size_t keep = std::min<size_t>(n, kMaxCapture - std::min(captured, kMaxCapture));
        if (captured < kMaxCapture) {
            if (keep > 0)
                buf.append(chunk, keep);
            captured += n;
        }
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (out) {
        if (captured > kMaxCapture) {
            std::string marker = "\n... [output truncated: " + std::to_string(captured) +
                                 " bytes total]\n";
            buf.append(marker);
        }
        *out = std::move(buf);
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
#else
    (void)argv; (void)cwd; (void)out; (void)env;
    return -1;
#endif
}

std::string to_upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r'))
        --e;
    return s.substr(b, e - b);
}

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace kiln::util

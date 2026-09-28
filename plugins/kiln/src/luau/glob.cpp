#include "luau/glob.h"

#include <algorithm>

#include "core/util.h"

namespace kiln::luau {

namespace {

// Expand {a,b} braces in a single path component (no nesting).
std::vector<std::string> expand_braces(const std::string& comp) {
    auto open = comp.find('{');
    if (open == std::string::npos)
        return {comp};
    auto close = comp.find('}', open);
    if (close == std::string::npos)
        return {comp}; // unmatched brace: literal
    std::string prefix = comp.substr(0, open);
    std::string suffix = comp.substr(close + 1);
    std::string body = comp.substr(open + 1, close - open - 1);
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= body.size()) {
        size_t comma = body.find(',', start);
        if (comma == std::string::npos)
            comma = body.size();
        out.push_back(prefix + body.substr(start, comma - start) + suffix);
        if (comma == body.size())
            break;
        start = comma + 1;
    }
    return out;
}

// Expand braces across all components (cartesian).
void expand_all(const std::vector<std::string>& comps, size_t i, std::string cur,
                std::vector<std::string>& out) {
    if (i == comps.size()) {
        out.push_back(cur);
        return;
    }
    for (auto& e : expand_braces(comps[i]))
        expand_all(comps, i + 1, cur.empty() ? e : cur + "/" + e, out);
}

// Match one path component against a pattern component (no '/', no '{').
bool match_component(std::string_view pat, std::string_view name) {
    size_t p = 0, n = 0;
    while (p < pat.size()) {
        char c = pat[p];
        if (c == '*') {
            ++p;
            if (p >= pat.size())
                return true;
            for (size_t k = n; k <= name.size(); ++k) {
                if (match_component(pat.substr(p), name.substr(k)))
                    return true;
            }
            return false;
        }
        if (c == '?') {
            if (n >= name.size())
                return false;
            ++p;
            ++n;
            continue;
        }
        if (n >= name.size() || name[n] != c)
            return false;
        ++p;
        ++n;
    }
    return n == name.size();
}

// Match a '/' -split path against pattern components, where "**" matches zero
// or more components.
bool match_path(const std::vector<std::string>& pats, size_t pi,
                const std::vector<std::string>& names, size_t ni) {
    while (pi < pats.size()) {
        if (pats[pi] == "**") {
            ++pi;
            if (pi == pats.size())
                return true;
            for (size_t k = ni; k <= names.size(); ++k) {
                if (match_path(pats, pi, names, k))
                    return true;
            }
            return false;
        }
        if (ni >= names.size())
            return false;
        if (!match_component(pats[pi], names[ni]))
            return false;
        ++pi;
        ++ni;
    }
    return ni == names.size();
}

} // namespace

Result<GlobResult> glob(const fs::path& root, std::string pattern) {
    pattern = util::norm(pattern);
    if (pattern.empty() || pattern == ".")
        pattern = "**/*";

    std::vector<std::string> comps;
    {
        std::string cur;
        for (char c : pattern) {
            if (c == '/') {
                comps.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        comps.push_back(cur);
    }
    // Strip leading "./" and handle ".." escapes by walking the walk-root up.
    int up = 0;
    while (!comps.empty() && (comps[0] == "." || comps[0] == ".." || comps[0].empty())) {
        if (comps[0] == "..")
            ++up;
        comps.erase(comps.begin());
    }
    if (comps.empty())
        return Result<GlobResult>::fail("invalid glob pattern: " + pattern);

    fs::path walk_root = root;
    for (int i = 0; i < up; ++i)
        walk_root = walk_root.parent_path();

    std::vector<std::string> patterns;
    expand_all(comps, 0, "", patterns);
    for (auto& p : patterns)
        p = util::norm(p);

    auto files = util::walk_files_rel(walk_root);
    if (!files.ok())
        return Result<GlobResult>::fail(files.error);

    std::vector<std::string> matched;
    for (auto& f : *files) {
        std::vector<std::string> fcomps;
        {
            std::string cur;
            for (char c : f) {
                if (c == '/') {
                    fcomps.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            fcomps.push_back(cur);
        }
        for (auto& p : patterns) {
            std::vector<std::string> pcomps;
            {
                std::string cur;
                for (char c : p) {
                    if (c == '/') {
                        pcomps.push_back(cur);
                        cur.clear();
                    } else {
                        cur.push_back(c);
                    }
                }
                pcomps.push_back(cur);
            }
            if (match_path(pcomps, 0, fcomps, 0)) {
                // Result is relative to the ORIGINAL root: prefix the ".." hops.
                std::string rel;
                for (int i = 0; i < up; ++i)
                    rel += "../";
                rel += f;
                matched.push_back(rel);
                break;
            }
        }
    }
    std::sort(matched.begin(), matched.end());
    GlobResult r;
    r.pattern = pattern;
    r.files = std::move(matched);
    return Result<GlobResult>::success(std::move(r));
}

} // namespace kiln::luau

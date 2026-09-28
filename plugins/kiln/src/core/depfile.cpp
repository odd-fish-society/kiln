#include "core/depfile.h"

#include <set>

namespace kiln {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

} // namespace

Result<std::vector<std::string>> parse_depfile(const std::string& content) {
    // Tokenize: split on unescaped whitespace/newlines. Backslash-newline is a
    // continuation (following whitespace ignored); '\ ' escapes a space;
    // '\\' is a literal backslash.
    std::vector<std::string> tokens;
    std::string cur;
    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];
        if (c == '\\' && i + 1 < content.size()) {
            char next = content[i + 1];
            if (next == '\n') {
                ++i; // swallow newline
                while (i + 1 < content.size() &&
                       (content[i + 1] == ' ' || content[i + 1] == '\t'))
                    ++i; // swallow continuation indentation
                continue;
            }
            if (next == ' ') {
                cur.push_back(' ');
                ++i;
                continue;
            }
            if (next == '\\') {
                cur.push_back('\\');
                ++i;
                continue;
            }
            if (next == '#' || next == ':' || next == '|') {
                cur.push_back(next);
                ++i;
                continue;
            }
            cur.push_back(c);
            continue;
        }
        if (is_space(c) || c == '\n') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty())
        tokens.push_back(cur);

    // First token(s) are targets (split by ':'); the rest are prerequisites.
    // Conventionally there is exactly one target: "out.o: dep1 dep2".
    std::vector<std::string> deps;
    bool seen_colon = false;
    for (auto& t : tokens) {
        auto colon = t.find(':');
        if (!seen_colon && colon != std::string::npos) {
            seen_colon = true;
            // Remainder after the colon is a dep glued to the target token.
            std::string rest = t.substr(colon + 1);
            if (!rest.empty())
                deps.push_back(rest);
            continue;
        }
        if (seen_colon)
            deps.push_back(t);
    }
    std::set<std::string> uniq(deps.begin(), deps.end());
    return Result<std::vector<std::string>>::success(
        std::vector<std::string>(uniq.begin(), uniq.end()));
}

} // namespace kiln

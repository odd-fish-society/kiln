#include "cli/cli_common.h"

#include <cstdio>
#include <string>
#include <vector>

#include "cluau/convert.h"
#include "core/util.h"

namespace kiln {

namespace {

// One file: convert, verify the conversion by translating it back, then write
// the .cluau beside it and remove the .luau. Nothing is written or removed
// unless every step passed.
bool convert_one(const fs::path& file, bool check_only) {
    const std::string shown = file.generic_string();
    if (file.extension() != ".luau") {
        std::fprintf(stderr, "kiln convert: %s: not a .luau file\n", shown.c_str());
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        std::fprintf(stderr, "kiln convert: %s: no such file\n", shown.c_str());
        return false;
    }
    fs::path target = file;
    target.replace_extension(".cluau");
    if (fs::exists(target, ec)) {
        std::fprintf(stderr, "kiln convert: %s: %s already exists, so nothing was converted\n", shown.c_str(),
                     target.generic_string().c_str());
        return false;
    }
    auto source = util::read_file(file);
    if (!source.ok()) {
        std::fprintf(stderr, "kiln convert: %s\n", source.error.c_str());
        return false;
    }
    cluau::ConvertResult converted = cluau::Convert(*source);
    if (converted.HasErrors()) {
        for (auto& d : converted.Diagnostics)
            if (!d.Warning)
                std::fprintf(stderr, "%s:%d:%d: error: %s\n", shown.c_str(), d.At.Begin.Line + 1,
                             d.At.Begin.Col + 1, d.Message.c_str());
        return false;
    }
    const std::string mismatch = cluau::VerifyConversion(*source, converted.Cluau);
    if (!mismatch.empty()) {
        std::fprintf(stderr, "kiln convert: %s: the conversion doesn't translate back to the same code (%s), so "
                             "nothing was written\n",
                     shown.c_str(), mismatch.c_str());
        return false;
    }
    if (check_only) {
        std::printf("%s -> %s\n", shown.c_str(), target.generic_string().c_str());
        return true;
    }
    auto wrote = util::write_file(target, converted.Cluau);
    if (!wrote.ok()) {
        std::fprintf(stderr, "kiln convert: %s\n", wrote.error.c_str());
        return false;
    }
    fs::remove(file, ec);
    if (ec) {
        std::fprintf(stderr, "kiln convert: wrote %s but could not remove %s: %s\n",
                     target.generic_string().c_str(), shown.c_str(), ec.message().c_str());
        return false;
    }
    std::printf("converted %s -> %s\n", shown.c_str(), target.generic_string().c_str());
    return true;
}

} // namespace

Result<int> cmd_convert(const std::vector<std::string>& files, bool check_only) {
    if (files.empty())
        return Result<int>::fail("kiln convert: name the .luau files to convert");
    int failed = 0;
    for (auto& f : files)
        failed += convert_one(fs::path(f), check_only) ? 0 : 1;
    if (failed)
        std::fprintf(stderr, "kiln convert: %d of %zu file(s) not converted\n", failed, files.size());
    return Result<int>::success(failed ? 1 : 0);
}

} // namespace kiln

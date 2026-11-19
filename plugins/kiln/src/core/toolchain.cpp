#include "core/toolchain.h"

#include <vector>

#include "core/hash.h"
#include "core/util.h"

namespace kiln {

namespace fs = std::filesystem;

namespace {

// A tool spelled with a '/' is a path (workspace-relative or absolute);
// a bare name is looked up on PATH, the way the executor's exec does.
fs::path resolve_tool(const std::string& tool, const fs::path& workspace_root) {
    if (tool.find('/') != std::string::npos) {
        fs::path p(tool);
        return p.is_relative() ? workspace_root / p : p;
    }
    return fs::path(util::find_in_path(tool));
}

bool hash_file_canonical(const fs::path& p, Hash256* out) {
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec))
        return false;
    fs::path real = fs::canonical(p, ec);
    auto h = Hash256::of_file(ec ? p : real);
    if (!h.ok())
        return false;
    *out = *h;
    return true;
}

} // namespace

std::string toolchain_fingerprint(const std::string& tool, const fs::path& workspace_root,
                                  bool probe_driver) {
    fs::path driver = resolve_tool(tool, workspace_root);
    Hash256 driver_hash;
    if (!hash_file_canonical(driver, &driver_hash))
        return "unresolved";

    CanonicalHasher h;
    h.field("driver", driver_hash);
    if (probe_driver) {
        std::string banner;
        int rc = util::run_process({driver.generic_string(), "--version"}, workspace_root, &banner);
        h.field("version", rc == 0 ? Hash256::of_string(banner) : Hash256::of_string(""));

        // Sorted, fixed probe list: the hashed order is the list order.
        h.begin_list("progs");
        for (const char* prog : {"cc1", "cc1plus", "ld"}) {
            std::string out;
            if (util::run_process({driver.generic_string(), std::string("-print-prog-name=") + prog},
                                  workspace_root, &out) != 0)
                continue;
            std::string name(util::trim(out));
            // GCC prints an absolute path for the programs it ships; a bare
            // name means "found on PATH" (ld) or "not mine" (clang's cc1plus).
            fs::path resolved = name.find('/') != std::string::npos
                                    ? fs::path(name)
                                    : fs::path(util::find_in_path(name));
            Hash256 ph;
            if (!hash_file_canonical(resolved, &ph))
                continue;
            h.item(prog);
            h.item(ph);
        }
        h.end_list();

        // The C++ runtime the driver links and compiles against: Clang has no
        // cc1plus of its own to change and uses the GCC installation's
        // libstdc++ and crt files, so a GCC upgrade under an unchanged Clang
        // must rerun its compiles (and rebuild its PCHs) too.
        h.begin_list("files");
        for (const char* file : {"crtbegin.o", "libstdc++.so"}) {
            std::string out;
            if (util::run_process({driver.generic_string(), std::string("-print-file-name=") + file},
                                  workspace_root, &out) != 0)
                continue;
            fs::path resolved(util::trim(out));
            Hash256 fh;
            if (!resolved.is_absolute() || !hash_file_canonical(resolved, &fh))
                continue;
            h.item(file);
            h.item(fh);
        }
        h.end_list();
    }
    return h.finish().hex();
}

} // namespace kiln

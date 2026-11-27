#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace kiln {

class FileHashCache;

// The toolchain fingerprint: the content identity of a
// compiler/archiver/linker driver, folded into its tasks' params as
// `toolchain_hash` so a toolchain upgrade reruns exactly the tasks it touches.
// Without it a PATH tool ("c++") is a bare name in the digest and depfile
// paths outside the workspace are dropped, so an upgrade was invisible: the
// stale-PCH `-Werror=invalid-pch` failures after GCC 16.1 -> 16.2.
//
// Covers the driver binary (symlinks resolved), and with `probe_driver` its
// `--version` banner plus the programs it runs (`-print-prog-name=` for
// cc1plus/cc1/ld) — the GCC driver does not change on every compiler rebuild,
// cc1plus does — and the runtime files it resolves (`-print-file-name=`
// crtbegin.o/libstdc++.so: a Clang driver uses the GCC installation's).
// Only file CONTENTS and the banner are hashed, never a path
// (spec §1). An unresolvable tool yields a fixed marker; the task then fails
// at run time naming the tool, as before.
//
// The files are hashed through `hashes` (the persisted (mtime, size)-keyed
// cache, like every source) when given: a toolchain is hundreds of MB, and
// re-reading it on every lowering was most of a no-op build.
std::string toolchain_fingerprint(const std::string& tool,
                                  const std::filesystem::path& workspace_root,
                                  bool probe_driver, FileHashCache* hashes = nullptr);

// Whether the driver is Clang (its `--version` banner says so).
bool toolchain_is_clang(const std::string& tool, const std::filesystem::path& workspace_root);

// The toolchain facts of one kiln run, shared by every target it lowers:
// each driver is probed (its processes spawned, its files hashed) once per
// run, not once per target.
struct ToolchainProbes {
    FileHashCache* hashes = nullptr;
    std::string fingerprint(const std::string& tool, const std::filesystem::path& workspace_root,
                            bool probe_driver);
    bool is_clang(const std::string& tool, const std::filesystem::path& workspace_root);

private:
    std::map<std::string, std::string> fingerprints_; // "<probe>:<tool>" -> fingerprint
    std::map<std::string, bool> clang_;
};

} // namespace kiln

#pragma once

#include <filesystem>
#include <string>

namespace kiln {

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
std::string toolchain_fingerprint(const std::string& tool,
                                  const std::filesystem::path& workspace_root,
                                  bool probe_driver);

} // namespace kiln

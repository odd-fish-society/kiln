-- CLI11 (vendored, header-only). One plugin per vendored library (spec
-- §7a.1): the tree lives inside the plugin dir beside the c_library decl.

plugin "cli11" {}

c_library "cli11" {
    sources = {},
    public_includes = { "." },
    cpp_std = "c++17",
}

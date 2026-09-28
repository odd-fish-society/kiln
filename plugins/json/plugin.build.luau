-- nlohmann/json (vendored, header-only). One plugin per vendored library
-- (spec §7a.1): the tree lives inside the plugin dir beside the decl.

plugin "json" {}

c_library "json" {
    sources = {},
    public_includes = { "." },
    cpp_std = "c++17",
}

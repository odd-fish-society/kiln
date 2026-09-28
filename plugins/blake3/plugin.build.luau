-- BLAKE3 (vendored, portable C). One plugin per vendored library (spec
-- §7a.1): sources inside the plugin dir beside the c_library decl.

plugin "blake3" {}

c_library "blake3" {
    sources = {
        "c/blake3.c",
        "c/blake3_dispatch.c",
        "c/blake3_portable.c",
    },
    public_includes = { "c" },
    defines = { "BLAKE3_NO_SSE2", "BLAKE3_NO_SSE41", "BLAKE3_NO_AVX2", "BLAKE3_NO_AVX512" },
    c_std = "c11",
}

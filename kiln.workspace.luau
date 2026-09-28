workspace "kiln" {
    pools = { link = 1 },
    strict_globs = true,
    cpp_std = "c++20",
    c_std = "c11",
    default_config = "release",
    system_libs = { "dl", "m" },
}

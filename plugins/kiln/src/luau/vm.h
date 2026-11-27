#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"

struct lua_State;

namespace kiln::luau {

// One fresh lua_State per expansion unit (spec §6.1). Sandboxed: explicit
// positive env only: base (minus print/loadstring), table, string,
// math (minus random/randomseed), bit32, plus the Kiln API installed by
// the caller. luaL_sandbox applied. TWO budgets, both required (spec §6.1):
//   - memory: 256 MiB live-heap cap enforced in the lua_Alloc callback
//     (returns NULL over cap ⇒ Luau raises a memory error); few instructions
//     can allocate unbounded memory, so the interrupt alone is insufficient.
//   - execution: safepoint-count interrupt via the Luau interrupt callback.
class VM {
public:
    struct Options {
        uint64_t interrupt_budget = 50000000;        // safepoint interrupts
        size_t memory_cap = 256ull * 1024 * 1024;    // live VM heap bytes
    };

    static Result<std::unique_ptr<VM>> create();
    static Result<std::unique_ptr<VM>> create(const Options& opts);
    ~VM();

    lua_State* state() { return L_; }

    // True when an allocation was refused because it would exceed memory_cap
    // (as opposed to a plain system OOM). Evaluate the pcall result together
    // with this to surface the friendly budget error.
    bool memory_cap_exceeded() const { return alloc_state_.cap_exceeded; }
    size_t memory_cap() const { return memory_cap_; }

    // Compiles Luau source to bytecode (malloc'd buffer, caller frees via
    // free_bytecode). Returns error on syntax problems.
    static Result<std::vector<char>> compile(const std::string& source);

    // Loads bytecode as a chunk named `name`; 0 on success, pushes error on
    // failure. Set name to the file path for error messages.
    int load(const std::vector<char>& bytecode, const std::string& name);

    // Runs the loaded chunk (0 args, 0 results); 0 on success.
    int run();

    // Opens the restricted libs + sandboxes the state. Exposed so the manifest
    // env layer can compose (this is the only lib-opening path).
    static void install_restricted_env(lua_State* L);

private:
    VM() = default;
    static void interrupt_cb(lua_State* L, int gc);
    static void* tracked_alloc(void* ud, void* ptr, size_t osize, size_t nsize);

    struct AllocState {
        size_t live_bytes = 0;
        size_t cap = 0;
        bool cap_exceeded = false;
        bool oom = false;
    };

    lua_State* L_ = nullptr;
    uint64_t interrupt_count_ = 0;
    uint64_t interrupt_budget_ = 0;
    size_t memory_cap_ = 0;
    AllocState alloc_state_; // live: the allocator callback writes cap_exceeded here
};

} // namespace kiln::luau

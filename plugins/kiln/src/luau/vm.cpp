#include "luau/vm.h"

#include <cstdlib>
#include <cstring>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

namespace kiln::luau {

namespace {

void remove_global(lua_State* L, const char* name) {
    lua_pushnil(L);
    lua_setglobal(L, name);
}

void remove_field(lua_State* L, const char* table, const char* name) {
    lua_getglobal(L, table);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushnil(L);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

} // namespace

void VM::install_restricted_env(lua_State* L) {
    // Positive env construction: open ONLY the allowed libs.
    lua_pushcfunction(L, luaopen_base, NULL);
    lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_table, NULL);
    lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_string, NULL);
    lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_math, NULL);
    lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_bit32, NULL);
    lua_call(L, 0, 0);

    remove_global(L, "print");
    remove_global(L, "getfenv");
    remove_global(L, "setfenv");
    remove_global(L, "newproxy");
    remove_field(L, "math", "random");
    remove_field(L, "math", "randomseed");
    remove_field(L, "math", "fmod");

    luaL_sandbox(L);
    luaL_sandboxthread(L);
}

void VM::interrupt_cb(lua_State* L, int gc) {
    (void)gc;
    // No stack operations at a safepoint: use the callbacks' userdata slot
    // ("arbitrary userdata pointer that is never overwritten by Luau").
    VM* self = static_cast<VM*>(lua_callbacks(L)->userdata);
    if (!self)
        return;
    if (++self->interrupt_count_ > self->interrupt_budget_) {
        // Safepoint context: VM_PROTECT makes raising here safe.
        luaL_error(L, "manifest exceeded execution budget");
    }
}

// Memory cap lives HERE, in the allocator (spec §6.1): the interrupt budget
// does not bound memory — string.rep and table growth allocate unboundedly in
// few instructions. Returning NULL over the cap makes Luau raise a memory
// error ("not enough memory"); the VM maps that to the friendly budget error
// when memory_cap_exceeded() is set.
void* VM::tracked_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* st = static_cast<AllocState*>(ud);
    if (nsize == 0) {
        std::free(ptr);
        st->live_bytes = st->live_bytes >= osize ? st->live_bytes - osize : 0;
        return nullptr;
    }
    if (ptr == nullptr) {
        if (nsize > st->cap - st->live_bytes) {
            st->cap_exceeded = true;
            return nullptr;
        }
        void* p = std::realloc(nullptr, nsize);
        if (!p) {
            st->oom = true;
            return nullptr;
        }
        st->live_bytes += nsize;
        return p;
    }
    if (nsize > osize) {
        size_t delta = nsize - osize;
        if (delta > st->cap - st->live_bytes) {
            st->cap_exceeded = true;
            return nullptr;
        }
        void* p = std::realloc(ptr, nsize);
        if (!p) {
            st->oom = true;
            return nullptr;
        }
        st->live_bytes += delta;
        return p;
    }
    void* p = std::realloc(ptr, nsize);
    if (!p && nsize != 0) {
        st->oom = true;
        return nullptr;
    }
    st->live_bytes = st->live_bytes >= (osize - nsize) ? st->live_bytes - (osize - nsize)
                                                       : 0;
    return p;
}

Result<std::unique_ptr<VM>> VM::create() { return create(Options{}); }

Result<std::unique_ptr<VM>> VM::create(const Options& opts) {
    auto vm = std::unique_ptr<VM>(new VM());
    vm->interrupt_budget_ = opts.interrupt_budget;
    vm->memory_cap_ = opts.memory_cap;
    vm->alloc_state_.cap = opts.memory_cap;
    vm->L_ = lua_newstate(&VM::tracked_alloc, &vm->alloc_state_);
    if (!vm->L_)
        return Result<std::unique_ptr<VM>>::fail("lua_newstate failed");
    install_restricted_env(vm->L_);

    lua_Callbacks* cb = lua_callbacks(vm->L_);
    cb->userdata = vm.get();
    cb->interrupt = &VM::interrupt_cb;
    return Result<std::unique_ptr<VM>>::success(std::move(vm));
}

VM::~VM() {
    if (L_)
        lua_close(L_);
}

Result<std::vector<char>> VM::compile(const std::string& source) {
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.coverageLevel = 0;
    size_t size = 0;
    char* bc = luau_compile(source.data(), source.size(), &options, &size);
    if (!bc)
        return Result<std::vector<char>>::fail("luau_compile failed");
    std::vector<char> out(bc, bc + size);
    std::free(bc);
    return Result<std::vector<char>>::success(std::move(out));
}

int VM::load(const std::vector<char>& bytecode, const std::string& name) {
    return luau_load(L_, name.c_str(), bytecode.data(), bytecode.size(), 0);
}

int VM::run() { return lua_pcall(L_, 0, 0, 0); }

} // namespace kiln::luau

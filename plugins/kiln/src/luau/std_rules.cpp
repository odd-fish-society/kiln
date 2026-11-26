#include "luau/std_rules.h"

#include "core/hash.h"

namespace kiln::luau {

namespace {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
constexpr char kCook[] = {
#embed "../../std/cook.cluau"
    , 0};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

constexpr StdLibrary kLibraries[] = {
    {"std/cook", "std/cook.cluau", std::string_view(kCook, sizeof(kCook) - 1)},
};

} // namespace

std::span<const StdLibrary> std_libraries() { return kLibraries; }

const std::string& std_libraries_hash() {
    static const std::string hash = [] {
        std::string all;
        for (auto& lib : kLibraries) {
            all += lib.name;
            all += '\n';
            all += std::to_string(lib.source.size());
            all += '\n';
            all += lib.source;
        }
        return Hash256::of_string(all).hex();
    }();
    return hash;
}

} // namespace kiln::luau

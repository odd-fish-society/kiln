#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "core/result.h"

namespace fs = std::filesystem;

namespace kiln {

// BLAKE3 output (32 bytes).
struct Hash256 {
    std::array<uint8_t, 32> bytes{};

    std::string hex() const;         // lowercase, 64 chars
    std::string short_hex() const;   // first 8 chars (display only, never in digests)

    static Hash256 of_bytes(std::span<const uint8_t> data);
    static Hash256 of_string(std::string_view s);
    // Missing/unreadable file ⇒ Result error (a distinguished error, never a zero hash).
    static Result<Hash256> of_file(const fs::path& p);

    auto operator<=>(const Hash256&) const = default;
    bool operator==(const Hash256&) const = default;
};

// The ONLY way structured data becomes a hash (spec §2.3).
// Keyed streaming BLAKE3 with unambiguous framing:
//   every field is len(key)||key||len(value)||value with 4-byte LE lengths;
//   lists frame each item the same way plus begin/end sentinels.
class CanonicalHasher {
public:
    CanonicalHasher();

    void field(std::string_view key, std::string_view val);
    void field(std::string_view key, uint64_t val);
    void field(std::string_view key, const Hash256& val);
    void begin_list(std::string_view key);
    void end_list();
    void item(std::string_view val);
    void item(const Hash256& val);

    Hash256 finish();

private:
    void frame(std::string_view key, std::span<const uint8_t> value);
    void raw_frame(std::span<const uint8_t> key, std::span<const uint8_t> value);

    void* impl_; // blake3_hasher
};

} // namespace kiln

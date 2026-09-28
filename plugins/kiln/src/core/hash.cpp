#include "core/hash.h"

#include <cstring>
#include <fstream>
#include <vector>

#include "blake3.h"

namespace kiln {

namespace {
constexpr size_t kChunk = 1 << 20; // 1 MiB streaming chunks
}

std::string Hash256::hex() const {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xf]);
    }
    return out;
}

std::string Hash256::short_hex() const { return hex().substr(0, 8); }

Hash256 Hash256::of_bytes(std::span<const uint8_t> data) {
    Hash256 h;
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data.data(), data.size());
    blake3_hasher_finalize(&hasher, h.bytes.data(), h.bytes.size());
    return h;
}

Hash256 Hash256::of_string(std::string_view s) {
    return of_bytes(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

Result<Hash256> Hash256::of_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return Result<Hash256>::fail("cannot open file for hashing: " + p.generic_string());
    std::vector<char> buf(kChunk);
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    while (f) {
        f.read(buf.data(), kChunk);
        auto n = f.gcount();
        if (n > 0)
            blake3_hasher_update(&hasher, buf.data(), static_cast<size_t>(n));
    }
    if (!f.eof())
        return Result<Hash256>::fail("read error while hashing: " + p.generic_string());
    Hash256 h;
    blake3_hasher_finalize(&hasher, h.bytes.data(), h.bytes.size());
    return Result<Hash256>::success(h);
}

CanonicalHasher::CanonicalHasher() {
    blake3_hasher* h = new blake3_hasher;
    blake3_hasher_init(h);
    impl_ = h;
}

void CanonicalHasher::raw_frame(std::span<const uint8_t> key,
                                std::span<const uint8_t> value) {
    auto* h = static_cast<blake3_hasher*>(impl_);
    uint8_t lenbuf[4];
    auto put_len = [&](uint32_t n) {
        lenbuf[0] = static_cast<uint8_t>(n & 0xff);
        lenbuf[1] = static_cast<uint8_t>((n >> 8) & 0xff);
        lenbuf[2] = static_cast<uint8_t>((n >> 16) & 0xff);
        lenbuf[3] = static_cast<uint8_t>((n >> 24) & 0xff);
        blake3_hasher_update(h, lenbuf, 4);
    };
    put_len(static_cast<uint32_t>(key.size()));
    if (!key.empty())
        blake3_hasher_update(h, key.data(), key.size());
    put_len(static_cast<uint32_t>(value.size()));
    if (!value.empty())
        blake3_hasher_update(h, value.data(), value.size());
}

void CanonicalHasher::frame(std::string_view key, std::span<const uint8_t> value) {
    raw_frame(std::span<const uint8_t>(
                  reinterpret_cast<const uint8_t*>(key.data()), key.size()),
              value);
}

void CanonicalHasher::field(std::string_view key, std::string_view val) {
    frame(key, std::span<const uint8_t>(
                   reinterpret_cast<const uint8_t*>(val.data()), val.size()));
}

void CanonicalHasher::field(std::string_view key, uint64_t val) {
    uint8_t buf[8];
    for (int i = 0; i < 8; ++i)
        buf[i] = static_cast<uint8_t>((val >> (8 * i)) & 0xff);
    frame(key, std::span<const uint8_t>(buf, 8));
}

void CanonicalHasher::field(std::string_view key, const Hash256& val) {
    frame(key, std::span<const uint8_t>(val.bytes));
}

void CanonicalHasher::begin_list(std::string_view key) { field(key, "["); }
void CanonicalHasher::end_list() { field("", "]"); }

void CanonicalHasher::item(std::string_view val) { field("", val); }
void CanonicalHasher::item(const Hash256& val) { field("", val); }

Hash256 CanonicalHasher::finish() {
    auto* h = static_cast<blake3_hasher*>(impl_);
    Hash256 out;
    blake3_hasher_finalize(h, out.bytes.data(), out.bytes.size());
    delete h;
    impl_ = nullptr;
    return out;
}

} // namespace kiln

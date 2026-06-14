#include "core/crypto/crypto.hpp"

#include "core/platform.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <monocypher.h>
}

#if defined(BISONDB_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    // <bcrypt.h> must follow <windows.h>.
    #include <bcrypt.h>
#endif

namespace bisondb::crypto {
namespace {

constexpr std::size_t kSaltBytes = 16;      // 128-bit salt
constexpr std::size_t kHashBytes = 32;      // 256-bit Argon2id output
constexpr std::size_t kTokenBytes = 32;     // 256-bit token
constexpr std::size_t kTokenHashBytes = 32; // BLAKE2b-256

std::string toHex(const std::uint8_t* data, std::size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0x0F];
    }
    return out;
}

// Decode a lowercase/uppercase hex string into bytes. Throws on bad input.
std::vector<std::uint8_t> fromHex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw CryptoError("invalid hex length");
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        int hi = nibble(hex[2 * i]);
        int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            throw CryptoError("invalid hex digit");
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return out;
}

// Run Argon2id over (password, salt) with the given cost; writes kHashBytes out.
void argon2id(std::string_view password, const std::uint8_t* salt, std::size_t saltLen,
              const KdfParams& p, std::uint8_t* outHash) {
    // Monocypher needs nb_blocks >= 8 * nb_lanes; the work area is 1 KiB/block.
    std::uint32_t lanes = p.lanes == 0 ? 1u : p.lanes;
    std::uint32_t blocks = p.memoryKiB;
    if (blocks < 8 * lanes) {
        blocks = 8 * lanes;
    }
    std::uint32_t passes = p.passes == 0 ? 1u : p.passes;

    std::vector<std::uint8_t> workArea(static_cast<std::size_t>(blocks) * 1024u);

    crypto_argon2_config config{};
    config.algorithm = CRYPTO_ARGON2_ID;
    config.nb_blocks = blocks;
    config.nb_passes = passes;
    config.nb_lanes = lanes;

    crypto_argon2_inputs inputs{};
    inputs.pass = reinterpret_cast<const std::uint8_t*>(password.data());
    inputs.pass_size = static_cast<std::uint32_t>(password.size());
    inputs.salt = salt;
    inputs.salt_size = static_cast<std::uint32_t>(saltLen);

    crypto_argon2_extras extras{}; // no key, no associated data

    crypto_argon2(outHash, static_cast<std::uint32_t>(kHashBytes), workArea.data(), config, inputs,
                  extras);

    crypto_wipe(workArea.data(), workArea.size());
}

} // namespace

void randomBytes(std::uint8_t* out, std::size_t len) {
    if (len == 0) {
        return;
    }
#if defined(BISONDB_PLATFORM_WINDOWS)
    NTSTATUS st =
        BCryptGenRandom(nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0) { // STATUS_SUCCESS == 0
        throw CryptoError("BCryptGenRandom failed");
    }
#else
    // /dev/urandom is the kernel CSPRNG and is universally available on POSIX.
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (f == nullptr) {
        throw CryptoError("cannot open /dev/urandom");
    }
    std::size_t got = std::fread(out, 1, len, f);
    std::fclose(f);
    if (got != len) {
        throw CryptoError("short read from /dev/urandom");
    }
#endif
}

std::string KdfParams::serialize() const {
    return "argon2id$m=" + std::to_string(memoryKiB) + ",t=" + std::to_string(passes) +
           ",p=" + std::to_string(lanes);
}

KdfParams KdfParams::parse(std::string_view s) {
    // Expected: argon2id$m=<u32>,t=<u32>,p=<u32>
    constexpr std::string_view prefix = "argon2id$";
    if (s.substr(0, prefix.size()) != prefix) {
        throw CryptoError("unsupported KDF id");
    }
    s.remove_prefix(prefix.size());

    KdfParams out{};
    bool haveM = false, haveT = false, haveP = false;
    while (!s.empty()) {
        std::size_t comma = s.find(',');
        std::string_view field = s.substr(0, comma);
        if (field.size() < 3 || field[1] != '=') {
            throw CryptoError("malformed KDF params");
        }
        char key = field[0];
        std::string num(field.substr(2));
        std::uint32_t value = 0;
        try {
            std::size_t consumed = 0;
            unsigned long parsed = std::stoul(num, &consumed);
            if (consumed != num.size() || parsed > 0xFFFFFFFFul) {
                throw CryptoError("bad KDF number");
            }
            value = static_cast<std::uint32_t>(parsed);
        } catch (const CryptoError&) {
            throw;
        } catch (const std::exception&) {
            throw CryptoError("bad KDF number");
        }
        switch (key) {
        case 'm':
            out.memoryKiB = value;
            haveM = true;
            break;
        case 't':
            out.passes = value;
            haveT = true;
            break;
        case 'p':
            out.lanes = value;
            haveP = true;
            break;
        default: throw CryptoError("unknown KDF field");
        }
        if (comma == std::string_view::npos) {
            break;
        }
        s.remove_prefix(comma + 1);
    }
    if (!haveM || !haveT || !haveP) {
        throw CryptoError("incomplete KDF params");
    }
    return out;
}

PasswordHash hashPassword(std::string_view password, KdfParams params) {
    std::array<std::uint8_t, kSaltBytes> salt{};
    randomBytes(salt.data(), salt.size());

    std::array<std::uint8_t, kHashBytes> hash{};
    argon2id(password, salt.data(), salt.size(), params, hash.data());

    PasswordHash out;
    out.hashHex = toHex(hash.data(), hash.size());
    out.saltHex = toHex(salt.data(), salt.size());
    out.params = params.serialize();
    crypto_wipe(hash.data(), hash.size());
    return out;
}

bool verifyPassword(std::string_view password, const PasswordHash& stored) noexcept {
    try {
        KdfParams params = KdfParams::parse(stored.params);
        std::vector<std::uint8_t> salt = fromHex(stored.saltHex);
        std::vector<std::uint8_t> expected = fromHex(stored.hashHex);
        if (expected.size() != kHashBytes) {
            return false;
        }
        std::array<std::uint8_t, kHashBytes> actual{};
        argon2id(password, salt.data(), salt.size(), params, actual.data());
        // crypto_verify32 is constant-time and returns 0 on equality.
        bool ok = crypto_verify32(actual.data(), expected.data()) == 0;
        crypto_wipe(actual.data(), actual.size());
        return ok;
    } catch (...) {
        return false; // malformed stored record fails closed
    }
}

std::string hashToken(std::string_view rawToken) {
    std::array<std::uint8_t, kTokenHashBytes> digest{};
    crypto_blake2b(digest.data(), digest.size(),
                   reinterpret_cast<const std::uint8_t*>(rawToken.data()), rawToken.size());
    return toHex(digest.data(), digest.size());
}

Token generateToken() {
    std::array<std::uint8_t, kTokenBytes> raw{};
    randomBytes(raw.data(), raw.size());
    Token t;
    t.raw = toHex(raw.data(), raw.size());
    t.hashHex = hashToken(t.raw);
    crypto_wipe(raw.data(), raw.size());
    return t;
}

bool constantTimeEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    volatile unsigned char acc = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        acc = static_cast<unsigned char>(
            acc | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return acc == 0;
}

} // namespace bisondb::crypto

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Crypto wrapper — the ONLY place the rest of BisonDB touches a crypto library.
//
// Backed by Monocypher (vetted C99: RFC 9106 Argon2id, BLAKE2b, constant-time
// compare) plus the OS CSPRNG for random bytes. Keeping every primitive behind
// this boundary means the planned TLS phase can reuse it and the engine never
// links a crypto API directly.
//
// SECURITY NOTES
//   * Passwords are hashed with Argon2id (memory-hard). Plaintext is never
//     stored or returned.
//   * Access tokens are 256-bit CSPRNG values; only a BLAKE2b-256 hash of the
//     token is ever persisted/compared server-side.
//   * Equal-length secret comparisons use constant-time compare.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bisondb::crypto {

// Thrown when the OS CSPRNG is unavailable or a stored hash/params blob is
// malformed. Never carries secret material in its message.
class CryptoError : public Error {
  public:
    using Error::Error;
};

// Fill `out` with `len` cryptographically secure random bytes from the OS CSPRNG
// (Windows: BCryptGenRandom; POSIX: /dev/urandom). Throws CryptoError on failure.
void randomBytes(std::uint8_t* out, std::size_t len);

// Tunable Argon2id cost. Stored next to each password hash so the cost can be
// raised over time without invalidating existing users.
struct KdfParams {
    std::uint32_t memoryKiB = 19456; // ~19 MiB — OWASP Argon2id floor
    std::uint32_t passes = 2;
    std::uint32_t lanes = 1;

    // "argon2id$m=<KiB>,t=<passes>,p=<lanes>"
    std::string serialize() const;
    static KdfParams parse(std::string_view s); // throws CryptoError on bad input
};

// Output of hashing a password. Every field is hex/ASCII, safe to store in BSON.
struct PasswordHash {
    std::string hashHex; // 32-byte Argon2id output, hex
    std::string saltHex; // 16-byte random salt, hex
    std::string params;  // KdfParams::serialize()
};

// Hash a password with a fresh random salt. Memory-hard; cost is `params`.
PasswordHash hashPassword(std::string_view password, KdfParams params = {});

// Constant-time verify of `password` against a stored hash. Returns false (never
// throws) if the stored blob is malformed, so a corrupt record fails closed.
bool verifyPassword(std::string_view password, const PasswordHash& stored) noexcept;

// A freshly minted access token. `raw` is handed to the client exactly once;
// only `hashHex` is persisted/compared server-side.
struct Token {
    std::string raw;     // 64-char hex (256 bits) — secret, never persisted
    std::string hashHex; // BLAKE2b-256(raw) hex — this is what the server stores
};

// Mint a 256-bit token from the CSPRNG.
Token generateToken();

// BLAKE2b-256 of a raw token's bytes, hex-encoded. hashToken(t.raw) == t.hashHex.
std::string hashToken(std::string_view rawToken);

// Constant-time equality. Unequal lengths return false immediately (length is
// not secret here); equal lengths are compared without an early-out branch.
bool constantTimeEquals(std::string_view a, std::string_view b) noexcept;

} // namespace bisondb::crypto

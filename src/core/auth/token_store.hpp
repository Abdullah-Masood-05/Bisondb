#pragma once

// In-memory session-token store, namespace bisondb::auth.
//
// Tokens are 256-bit CSPRNG values. The raw token is returned to the client
// exactly once (on issue); the store keeps only a BLAKE2b-256 hash of it, keyed
// in a map. Looking a token up means hashing the presented value and indexing
// by that hash — so the secret itself is never compared in a data-dependent
// branch, and the map comparison operates on the (non-secret-recoverable) hash.
//
// Tokens are session-scoped: they live only in memory and are lost on restart.
// That is intentional for this phase and documented in the security page.

#include "core/auth/roles.hpp"
#include "core/crypto/crypto.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bisondb::auth {

struct TokenInfo {
    std::string username;
    std::vector<Role> roles;
    std::int64_t issuedAtMs = 0;
    std::int64_t expiresAtMs = 0; // absolute epoch ms
};

class TokenStore {
  public:
    enum class Lookup {
        Valid,
        Expired,
        NotFound,
    };

    // Mint a token for (username, roles) valid for ttlSeconds from nowMs.
    // Returns the raw token (hand to the client); only its hash is retained.
    crypto::Token issue(const std::string& username, const std::vector<Role>& roles,
                        std::int64_t ttlSeconds, std::int64_t nowMs);

    // Resolve a raw token. On Valid, fills `out`. Expired tokens are evicted.
    Lookup resolve(std::string_view rawToken, std::int64_t nowMs, TokenInfo& out);

    // Resolve by an already-computed token hash. The server keeps only the hash
    // on a connection, so per-command re-validation (catching revocation and
    // expiry) goes through this path.
    Lookup resolveByHash(const std::string& tokenHash, std::int64_t nowMs, TokenInfo& out);

    // Invalidate a single token (logout). Returns true if it existed.
    bool revoke(std::string_view rawToken);
    bool revokeByHash(const std::string& tokenHash);

    // Invalidate every token belonging to a user (on drop/disable/pw-change).
    std::size_t revokeAllForUser(const std::string& username);

    std::size_t purgeExpired(std::int64_t nowMs);
    std::size_t size();

  private:
    std::mutex mutex_;
    std::unordered_map<std::string, TokenInfo> byHash_; // tokenHash -> info
};

} // namespace bisondb::auth

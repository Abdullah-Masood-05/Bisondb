#pragma once

// Per-connection authentication state and the brute-force backoff policy.
// A ConnectionAuth lives on the serving thread's stack for the lifetime of one
// TCP connection, so it needs no locking of its own.

#include "core/auth/roles.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bisondb::server {

// Connection auth state machine:
//   fresh connection  -> authenticated == false  (UNAUTHENTICATED)
//   after authenticate/authenticateToken -> authenticated == true (AUTHENTICATED)
//   token expiry / logout / revoke -> back to UNAUTHENTICATED
struct ConnectionAuth {
    bool authenticated = false;
    std::string username;
    std::vector<auth::Role> roles;
    std::int64_t expiresAtMs = 0; // absolute epoch ms of the backing token
    std::string tokenHash;        // BLAKE2b hash of the session token (for revoke)
    int failedAttempts = 0;       // consecutive failed authenticate attempts
};

// Per-connection throttle to blunt online password guessing. The first couple
// of failures are free (typos); after that, linear backoff capped at 2s. Pure
// function so it is unit-tested directly.
inline int authBackoffMs(int failedAttempts) {
    if (failedAttempts <= 2) {
        return 0;
    }
    int ms = (failedAttempts - 2) * 250;
    return ms > 2000 ? 2000 : ms;
}

} // namespace bisondb::server

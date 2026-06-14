#include "core/auth/token_store.hpp"

namespace bisondb::auth {

crypto::Token TokenStore::issue(const std::string& username, const std::vector<Role>& roles,
                                std::int64_t ttlSeconds, std::int64_t nowMs) {
    crypto::Token token = crypto::generateToken();
    TokenInfo info;
    info.username = username;
    info.roles = roles;
    info.issuedAtMs = nowMs;
    info.expiresAtMs = nowMs + ttlSeconds * 1000;
    std::lock_guard lock(mutex_);
    byHash_[token.hashHex] = std::move(info);
    return token;
}

TokenStore::Lookup TokenStore::resolve(std::string_view rawToken, std::int64_t nowMs,
                                       TokenInfo& out) {
    return resolveByHash(crypto::hashToken(rawToken), nowMs, out);
}

TokenStore::Lookup TokenStore::resolveByHash(const std::string& tokenHash, std::int64_t nowMs,
                                             TokenInfo& out) {
    std::lock_guard lock(mutex_);
    auto it = byHash_.find(tokenHash);
    if (it == byHash_.end()) {
        return Lookup::NotFound;
    }
    if (nowMs >= it->second.expiresAtMs) {
        byHash_.erase(it);
        return Lookup::Expired;
    }
    out = it->second;
    return Lookup::Valid;
}

bool TokenStore::revoke(std::string_view rawToken) {
    return revokeByHash(crypto::hashToken(rawToken));
}

bool TokenStore::revokeByHash(const std::string& tokenHash) {
    std::lock_guard lock(mutex_);
    return byHash_.erase(tokenHash) != 0;
}

std::size_t TokenStore::revokeAllForUser(const std::string& username) {
    std::lock_guard lock(mutex_);
    std::size_t removed = 0;
    for (auto it = byHash_.begin(); it != byHash_.end();) {
        if (it->second.username == username) {
            it = byHash_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t TokenStore::purgeExpired(std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    std::size_t removed = 0;
    for (auto it = byHash_.begin(); it != byHash_.end();) {
        if (nowMs >= it->second.expiresAtMs) {
            it = byHash_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t TokenStore::size() {
    std::lock_guard lock(mutex_);
    return byHash_.size();
}

} // namespace bisondb::auth

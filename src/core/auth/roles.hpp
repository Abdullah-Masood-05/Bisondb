#pragma once

// Roles and capabilities for the auth layer. Kept header-only and dependency-
// free so both the user store and the server's command guard share one source
// of truth for "what can this role do".

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bisondb::auth {

// Built-in roles, ordered from least to most privileged. Extensible later, but
// kept deliberately small for this phase.
enum class Role {
    Read,      // find / explain / listCollections / listIndexes / dbStats
    ReadWrite, // all data commands on all collections
    Admin,     // everything, incl. user management + shutdown
};

// The privilege a command requires. The dispatcher maps each command to one of
// these; a connection's roles must grant it.
enum class Capability {
    None,  // always allowed once authenticated (e.g. ping, logout, whoami)
    Read,  // read-only data/metadata
    Write, // mutating data/schema
    Admin, // user management, shutdown
};

inline std::string_view roleName(Role r) {
    switch (r) {
    case Role::Read: return "read";
    case Role::ReadWrite: return "readWrite";
    case Role::Admin: return "admin";
    }
    return "read";
}

inline std::optional<Role> parseRole(std::string_view s) {
    if (s == "read")
        return Role::Read;
    if (s == "readWrite")
        return Role::ReadWrite;
    if (s == "admin")
        return Role::Admin;
    return std::nullopt;
}

// Does a single role grant a capability?
inline bool roleGrants(Role role, Capability cap) {
    switch (cap) {
    case Capability::None: return true;
    case Capability::Read: return true; // every role can read
    case Capability::Write: return role == Role::ReadWrite || role == Role::Admin;
    case Capability::Admin: return role == Role::Admin;
    }
    return false;
}

// Does any role in the set grant the capability?
inline bool rolesGrant(const std::vector<Role>& roles, Capability cap) {
    for (Role r : roles) {
        if (roleGrants(r, cap)) {
            return true;
        }
    }
    return false;
}

inline bool rolesContainAdmin(const std::vector<Role>& roles) {
    for (Role r : roles) {
        if (r == Role::Admin) {
            return true;
        }
    }
    return false;
}

} // namespace bisondb::auth

#pragma once

// Persistent user store, namespace bisondb::auth.
//
// Users live in a dedicated append-only file <dbdir>/__auth.bsd, reusing the
// Phase-2 record framing (u8 type | u32 LE len | BSON payload). It is NOT a
// normal collection: it never appears in listCollections/dbStats and is never
// reachable through find/insert/export. Plaintext passwords are never stored —
// each record carries an Argon2id hash, its salt, and the KDF params.

#include "core/auth/roles.hpp"
#include "core/crypto/crypto.hpp"
#include "core/error.hpp"
#include "core/value.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bisondb::auth {

class AuthStoreError : public Error {
  public:
    using Error::Error;
};

// One user as persisted in __auth.bsd.
struct UserRecord {
    std::string username;
    crypto::PasswordHash password; // hashHex + saltHex + params
    std::vector<Role> roles;
    std::int64_t createdAtMs = 0;
    bool disabled = false;
};

// Valid usernames: [A-Za-z0-9_][A-Za-z0-9_.-]{0,63}. No whitespace/control.
bool isValidUsername(std::string_view name);

class UserStore {
  public:
    // Opens (replaying) <dbdir>/__auth.bsd, creating it on first write.
    // `kdf` is the cost used for new/changed passwords — tests pass a cheap
    // cost; production uses the default (~19 MiB Argon2id).
    explicit UserStore(std::string dbdir, crypto::KdfParams kdf = {});
    ~UserStore();

    UserStore(const UserStore&) = delete;
    UserStore& operator=(const UserStore&) = delete;

    bool empty();
    std::size_t userCount();
    std::size_t adminCount();
    bool exists(const std::string& username);
    std::optional<UserRecord> get(const std::string& username);
    std::vector<UserRecord> list();

    // Creates a user. Throws AuthStoreError on invalid name, duplicate, empty
    // password, or no/invalid roles.
    void createUser(const std::string& username, std::string_view password,
                    const std::vector<Role>& roles);

    // Removes a user. Returns false if absent. Throws AuthStoreError if this
    // would delete the last remaining admin (anti-lockout).
    bool dropUser(const std::string& username);

    bool setDisabled(const std::string& username, bool disabled);

    // Replaces a user's password (admin reset, or self-service after the
    // caller has checked the old password). Returns false if absent.
    bool setPassword(const std::string& username, std::string_view newPassword);

    // Constant-time-ish credential check. Returns the record only when the
    // user exists, is enabled, and the password verifies. To avoid user
    // enumeration via timing, an Argon2id verification is always performed
    // (against a dummy hash for unknown users).
    std::optional<UserRecord> verify(const std::string& username, std::string_view password);

  private:
    void replay();
    void appendPut(const UserRecord& rec);
    void appendDelete(const std::string& username);
    void openFile();

    Value toDocument(const UserRecord& rec) const;
    UserRecord fromDocument(const Value& doc) const;

    std::string path_;
    crypto::KdfParams kdf_;
    std::mutex mutex_;
    std::map<std::string, UserRecord> users_;
    std::FILE* file_ = nullptr;
    // A throwaway hash used to spend ~equal time when the user doesn't exist.
    crypto::PasswordHash dummyHash_;
};

} // namespace bisondb::auth

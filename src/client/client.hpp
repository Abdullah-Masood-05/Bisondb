#pragma once

#include "core/error.hpp"
#include "core/net/socket.hpp"
#include "core/net/tls.hpp"
#include "core/value.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bisondb::client {

// An { ok:false } response from the server, carrying its error code.
class ServerError : public Error {
  public:
    ServerError(std::string code, const std::string& message)
        : Error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

  private:
    std::string code_;
};

// Authentication/authorization failures, distinct from ServerError so callers
// can react to login problems specifically. Codes: AuthFailed, AuthRequired,
// TokenExpired, Forbidden.
class AuthError : public Error {
  public:
    AuthError(std::string code, const std::string& message)
        : Error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

  private:
    std::string code_;
};

// How to authenticate at connect time: username+password, or a previously
// obtained token.
struct Credentials {
    std::string username;
    std::string password;
    std::string token; // when non-empty, resume by token instead of password
    bool usesToken() const { return !token.empty(); }
};

struct UserInfo {
    std::string username;
    std::vector<std::string> roles;
    bool disabled = false;
};

// TLS options for connecting. When `enabled`, the TCP connection is wrapped in
// a TLS session before the auth handshake. Verification is secure by default
// (System trust + hostname); CaFile/Pin are the self-signed paths; Insecure
// skips verification and the caller must warn.
struct TlsOptions {
    bool enabled = false;
    net::TlsVerify verify = net::TlsVerify::System;
    std::string caFile;    // for CaFile
    std::string pinSha256; // for Pin
    std::string hostname;  // SNI + verification target; defaults to host
};

struct FindOptions {
    std::size_t limit = 0;
    std::size_t skip = 0;
    // When true, return exactly one server response without following
    // truncated batches.
    bool singleBatch = false;
};

// One client = one connection = one outstanding request. NOT thread-safe:
// use one BisonClient per thread. Transport failures throw net::NetError;
// { ok:false } responses throw ServerError.
class BisonClient {
  public:
    static BisonClient connect(const std::string& host, uint16_t port, int timeoutMs = 5000);

    // Connect and authenticate in one step. Throws AuthError on bad creds.
    static BisonClient connect(const std::string& host, uint16_t port, const Credentials& creds,
                               int timeoutMs = 5000);

    // TLS variants. The TCP connection is wrapped in TLS (verified per `tls`)
    // before any frame is sent; net::TlsError is thrown on handshake/verify
    // failure (with a hint when the server looks like plaintext).
    static BisonClient connect(const std::string& host, uint16_t port, const TlsOptions& tls,
                               int timeoutMs = 5000);
    static BisonClient connect(const std::string& host, uint16_t port, const TlsOptions& tls,
                               const Credentials& creds, int timeoutMs = 5000);

    // Sends one request document and returns the (ok:true) response payload.
    // If the session token has expired and password credentials are known,
    // transparently re-authenticates once and retries.
    Value command(Value request);

    // ── Authentication ────────────────────────────────────────────────────
    // Log in with a password; stores the returned token and roles. Throws
    // AuthError on failure. Returns the granted roles.
    std::vector<std::string> authenticate(const std::string& username, const std::string& password);
    // Resume a session from a token. Throws AuthError if invalid/expired.
    std::vector<std::string> authenticateToken(const std::string& token);
    void logout();

    // First-run only: create the first admin using the bootstrap token printed
    // by the server in setup mode. Leaves the connection authenticated.
    std::vector<std::string> bootstrapAdmin(const std::string& bootstrapToken,
                                            const std::string& username,
                                            const std::string& password);

    // User management (admin, except changePassword on self).
    void createUser(const std::string& username, const std::string& password,
                    const std::vector<std::string>& roles);
    bool dropUser(const std::string& username);
    std::vector<UserInfo> listUsers();
    // Self-service: pass oldPassword, leave targetUser empty. Admin reset:
    // set targetUser, oldPassword ignored.
    void changePassword(const std::string& newPassword, const std::string& oldPassword = "",
                        const std::string& targetUser = "");

    // Connection auth info.
    bool authenticated() const noexcept { return authenticated_; }
    const std::string& currentUser() const noexcept { return username_; }
    const std::vector<std::string>& currentRoles() const noexcept { return roles_; }
    const std::string& sessionToken() const noexcept { return token_; }

    // Connection transport info (for the shell's lock indicator).
    bool isTls() const noexcept { return tls_; }
    bool tlsVerified() const noexcept { return tlsVerified_; }

    void ping();
    Value serverStatus();
    std::vector<std::string> listCollections();
    bool createCollection(const std::string& coll); // false when it already exists
    Value dbStats();
    bool dropCollection(const std::string& coll);
    std::vector<ObjectId> insert(const std::string& coll, const std::vector<Value>& documents);
    // Follows truncated responses (skipNext) unless options.singleBatch.
    std::vector<Value> find(const std::string& coll, const Value& filter,
                            const FindOptions& options = {});
    std::size_t deleteMany(const std::string& coll, const Value& filter);
    bool updateOne(const std::string& coll, const Value& filter, const Value& update);
    int64_t createIndex(const std::string& coll, const std::string& field);
    void dropIndex(const std::string& coll, const std::string& field);
    std::vector<std::string> listIndexes(const std::string& coll);
    Value explain(const std::string& coll, const Value& filter, std::size_t limit = 0);
    void compact(const std::string& coll);
    void shutdownServer();

  private:
    explicit BisonClient(std::unique_ptr<net::Stream> stream) : stream_(std::move(stream)) {}

    // TCP connect + optional TLS handshake; returns a client with stream_ set.
    static BisonClient establish(const std::string& host, uint16_t port, const TlsOptions& tls,
                                 int timeoutMs);

    // One request/response exchange with no auto-reauth (used by the auth
    // handshake itself, and by command() internally).
    Value sendOnce(const Value& request);

    std::unique_ptr<net::Stream> stream_;
    bool tls_ = false;
    bool tlsVerified_ = false;

    // Session state for transparent re-auth and connection info.
    bool authenticated_ = false;
    std::string username_;
    std::string password_; // retained for token refresh; empty if token-only
    std::string token_;
    std::vector<std::string> roles_;
};

} // namespace bisondb::client

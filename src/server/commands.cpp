#include "core/auth/roles.hpp"
#include "core/auth/user_store.hpp"
#include "core/bson_encoder.hpp"
#include "core/btree/btree.hpp"
#include "core/btree/key_codec.hpp"
#include "core/query/query.hpp"
#include "core/version.hpp"
#include "server/auth_session.hpp"
#include "server/server.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

namespace bisondb::server {

namespace {

class CommandError : public Error {
  public:
    CommandError(std::string code, const std::string& message)
        : Error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

  private:
    std::string code_;
};

Value okResponse(Document payload = {}) {
    Document d{{"ok", Value(true)}};
    for (auto& [k, v] : payload) {
        d.append(k, std::move(v));
    }
    return Value(std::move(d));
}

Value errorResponse(const std::string& code, const std::string& message) {
    return Value(
        Document{{"ok", Value(false)},
                 {"error", Value(Document{{"code", Value(code)}, {"message", Value(message)}})}});
}

// ---- argument validation -------------------------------------------------

const Value& require(const Document& req, const char* field) {
    const Value* v = req.find(field);
    if (v == nullptr) {
        throw CommandError("BadRequest", std::string("missing required field \"") + field + "\"");
    }
    return *v;
}

const std::string& requireString(const Document& req, const char* field) {
    const Value& v = require(req, field);
    if (!v.is<std::string>()) {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be a string");
    }
    return v.get<std::string>();
}

const Document& requireDoc(const Document& req, const char* field) {
    const Value& v = require(req, field);
    if (!v.is<Document>()) {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be a document");
    }
    return v.asDocument();
}

const Array& requireArray(const Document& req, const char* field) {
    const Value& v = require(req, field);
    if (!v.is<Array>()) {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be an array");
    }
    return v.asArray();
}

std::size_t optionalCount(const Document& req, const char* field, std::size_t fallback) {
    const Value* v = req.find(field);
    if (v == nullptr) {
        return fallback;
    }
    int64_t n = 0;
    if (v->is<int32_t>()) {
        n = v->get<int32_t>();
    } else if (v->is<int64_t>()) {
        n = v->get<int64_t>();
    } else {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be an integer");
    }
    if (n < 0) {
        throw CommandError("BadRequest", std::string("field \"") + field + "\" must be >= 0");
    }
    return static_cast<std::size_t>(n);
}

query::IndexedCollection& collOf(Server& server, const Document& req) {
    return server.database().collection(requireString(req, "coll"));
}

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Parses a non-empty "roles" array of known role names. Throws on unknown role
// or an empty/missing list.
std::vector<auth::Role> requireRoles(const Document& req) {
    const Array& arr = requireArray(req, "roles");
    if (arr.empty()) {
        throw CommandError("BadRequest", "\"roles\" must not be empty");
    }
    std::vector<auth::Role> roles;
    for (const Value& v : arr) {
        if (!v.is<std::string>()) {
            throw CommandError("BadRequest", "\"roles\" entries must be strings");
        }
        auto r = auth::parseRole(v.get<std::string>());
        if (!r) {
            throw CommandError("BadRequest", "unknown role \"" + v.get<std::string>() + "\"");
        }
        roles.push_back(*r);
    }
    return roles;
}

Value rolesToArray(const std::vector<auth::Role>& roles) {
    Array out;
    for (auth::Role r : roles) {
        out.push_back(Value(std::string(auth::roleName(r))));
    }
    return Value(std::move(out));
}

// The single command -> required-capability table. Commands handled entirely
// before the gate (ping, serverStatus, authenticate*, logout, whoami,
// changePassword) map to None; changePassword enforces self-vs-admin itself.
auth::Capability capabilityFor(const std::string& cmd) {
    if (cmd == "find" || cmd == "explain" || cmd == "listCollections" || cmd == "listIndexes" ||
        cmd == "dbStats") {
        return auth::Capability::Read;
    }
    if (cmd == "insert" || cmd == "updateOne" || cmd == "deleteMany" || cmd == "createCollection" ||
        cmd == "dropCollection" || cmd == "createIndex" || cmd == "dropIndex" || cmd == "compact") {
        return auth::Capability::Write;
    }
    if (cmd == "createUser" || cmd == "dropUser" || cmd == "listUsers" || cmd == "shutdown") {
        return auth::Capability::Admin;
    }
    return auth::Capability::None;
}

// ---- handlers --------------------------------------------------------------

Value cmdInsert(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    const Array& docs = requireArray(req, "documents");
    if (docs.empty()) {
        throw CommandError("BadRequest", "\"documents\" must not be empty");
    }
    Array insertedIds;
    for (const Value& doc : docs) {
        if (!doc.is<Document>()) {
            throw CommandError("BadRequest", "\"documents\" entries must be documents");
        }
        insertedIds.push_back(Value(coll.insert(doc)));
    }
    return okResponse(Document{{"insertedIds", Value(std::move(insertedIds))},
                               {"insertedCount", Value(static_cast<int64_t>(docs.size()))}});
}

Value cmdFind(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::QueryEngine engine(coll);
    query::FindOptions opts;
    opts.limit = optionalCount(req, "limit", 0);
    opts.skip = optionalCount(req, "skip", 0);
    const Document& filter = requireDoc(req, "filter");
    std::vector<Value> docs = engine.find(Value(filter), opts);

    // One response must fit in maxMessageSize: include documents while a
    // byte budget lasts and tell the client where to resume (skipNext) when
    // truncated. Real cursors are out of scope (documented in protocol.md).
    std::size_t budget = server.config().maxMessageSize - 4096;
    Array included;
    std::size_t usedBytes = 0;
    bool truncated = false;
    for (Value& doc : docs) {
        std::size_t size = encodeDocument(doc).size();
        if (!included.empty() && usedBytes + size > budget) {
            truncated = true;
            break;
        }
        usedBytes += size;
        included.push_back(std::move(doc));
    }
    Document payload{{"documents", Value(std::move(included))}};
    std::size_t count = payload.find("documents")->asArray().size();
    payload.append("count", Value(static_cast<int64_t>(count)));
    if (truncated) {
        payload.append("truncated", Value(true));
        payload.append("skipNext", Value(static_cast<int64_t>(opts.skip + count)));
    }
    return okResponse(std::move(payload));
}

Value cmdDeleteMany(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::QueryEngine engine(coll);
    std::size_t n = engine.deleteMany(Value(requireDoc(req, "filter")));
    return okResponse(Document{{"deletedCount", Value(static_cast<int64_t>(n))}});
}

Value cmdUpdateOne(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::QueryEngine engine(coll);
    bool matched =
        engine.updateOne(Value(requireDoc(req, "filter")), Value(requireDoc(req, "update")));
    return okResponse(Document{{"matched", Value(matched)}, {"modified", Value(matched)}});
}

Value cmdCreateIndex(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::IndexBuildStats stats = coll.createIndex(requireString(req, "field"));
    return okResponse(Document{{"built", Value(true)},
                               {"docsIndexed", Value(static_cast<int64_t>(stats.indexed))},
                               {"docsSkipped", Value(static_cast<int64_t>(stats.skipped))}});
}

Value cmdExplain(Server& server, const Document& req) {
    query::IndexedCollection& coll = collOf(server, req);
    query::QueryEngine engine(coll);
    query::FindOptions opts;
    opts.limit = optionalCount(req, "limit", 0);
    query::ExplainResult result = engine.explain(Value(requireDoc(req, "filter")), opts);
    return okResponse(Document{{"plan", result.toValue()}});
}

// serverStatus is reachable before authentication, so the pre-auth view must
// expose nothing sensitive: just identity, protocol version, and the security
// posture. Authenticated callers additionally get uptime/connections/op stats.
Value cmdServerStatus(Server& server, const ConnectionAuth& conn) {
    Document security{{"auth", Value(server.authActive())},
                      {"tls", Value(server.tlsEnabled())},
                      {"setupMode", Value(server.inSetupMode())}};

    Document d{{"name", Value("BisonDB")},
               {"version", Value(std::string(version()))},
               {"protocolVersion", Value(kProtocolVersion)},
               {"security", Value(std::move(security))}};

    const bool privileged = !server.authActive() || conn.authenticated;
    if (privileged) {
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - server.stats().startedAt)
                          .count();
        Document counters;
        {
            std::lock_guard lock(server.stats().opMutex);
            for (const auto& [cmd, count] : server.stats().opCounters) {
                counters.append(cmd, Value(static_cast<int64_t>(count)));
            }
        }
        d.append("uptimeSec", Value(static_cast<int64_t>(uptime)));
        d.append("connectionsCurrent",
                 Value(static_cast<int64_t>(server.stats().connectionsCurrent.load())));
        d.append("opCounters", Value(std::move(counters)));
    }
    return okResponse(std::move(d));
}

// ---- auth handshake + user management handlers ----------------------------

Value cmdAuthenticate(Server& server, const Document& req, ConnectionAuth& conn) {
    if (!server.authActive()) {
        throw CommandError("BadRequest", "authentication is disabled (--no-auth)");
    }
    // Throttle before doing any work, scaled to this connection's failures.
    if (server.config().throttleAuth) {
        int delay = authBackoffMs(conn.failedAttempts);
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
    std::string username = requireString(req, "username");
    std::string password = requireString(req, "password");

    auto rec = server.users().verify(username, password);
    if (!rec) {
        ++conn.failedAttempts;
        server.auditLog("auth  login FAILED user=" + username);
        // Generic message: never reveal whether the username exists.
        throw CommandError("AuthFailed", "authentication failed");
    }
    conn.failedAttempts = 0;
    std::int64_t now = nowMs();
    crypto::Token token =
        server.tokens().issue(username, rec->roles, server.config().tokenTtlSeconds, now);

    conn.authenticated = true;
    conn.username = username;
    conn.roles = rec->roles;
    conn.expiresAtMs = now + server.config().tokenTtlSeconds * 1000;
    conn.tokenHash = token.hashHex;
    server.auditLog("auth  login ok user=" + username);

    return okResponse(Document{{"token", Value(token.raw)},
                               {"expiresInSec", Value(server.config().tokenTtlSeconds)},
                               {"username", Value(username)},
                               {"roles", rolesToArray(rec->roles)}});
}

Value cmdAuthenticateToken(Server& server, const Document& req, ConnectionAuth& conn) {
    if (!server.authActive()) {
        throw CommandError("BadRequest", "authentication is disabled (--no-auth)");
    }
    std::string raw = requireString(req, "token");
    auth::TokenInfo info;
    std::int64_t now = nowMs();
    switch (server.tokens().resolve(raw, now, info)) {
    case auth::TokenStore::Lookup::Expired:
        throw CommandError("TokenExpired", "session token expired; re-authenticate");
    case auth::TokenStore::Lookup::NotFound:
        throw CommandError("AuthFailed", "authentication failed");
    case auth::TokenStore::Lookup::Valid: break;
    }
    conn.authenticated = true;
    conn.username = info.username;
    conn.roles = info.roles;
    conn.expiresAtMs = info.expiresAtMs;
    conn.tokenHash = crypto::hashToken(raw);
    server.auditLog("auth  resume ok user=" + info.username);
    return okResponse(
        Document{{"username", Value(info.username)}, {"roles", rolesToArray(info.roles)}});
}

Value cmdLogout(Server& server, ConnectionAuth& conn) {
    if (conn.authenticated && !conn.tokenHash.empty()) {
        server.tokens().revokeByHash(conn.tokenHash);
    }
    server.auditLog("auth  logout user=" + conn.username);
    conn = ConnectionAuth{}; // back to UNAUTHENTICATED
    return okResponse();
}

Value cmdCreateUser(Server& server, const Document& req) {
    std::string username = requireString(req, "username");
    std::string password = requireString(req, "password");
    std::vector<auth::Role> roles = requireRoles(req);
    server.users().createUser(username, password, roles);
    server.auditLog("auth  user created: " + username);
    return okResponse();
}

Value cmdDropUser(Server& server, const Document& req, const ConnectionAuth& conn) {
    std::string username = requireString(req, "username");
    if (username == conn.username) {
        throw CommandError("BadRequest", "cannot drop the currently authenticated user");
    }
    bool dropped = server.users().dropUser(username);
    if (dropped) {
        server.tokens().revokeAllForUser(username);
        server.auditLog("auth  user dropped: " + username);
    }
    return okResponse(Document{{"dropped", Value(dropped)}});
}

Value cmdListUsers(Server& server) {
    Array users;
    for (const auth::UserRecord& rec : server.users().list()) {
        users.push_back(Value(Document{{"username", Value(rec.username)},
                                       {"roles", rolesToArray(rec.roles)},
                                       {"disabled", Value(rec.disabled)}}));
    }
    return okResponse(Document{{"users", Value(std::move(users))}});
}

Value cmdChangePassword(Server& server, const Document& req, const ConnectionAuth& conn) {
    std::string newPassword = requireString(req, "newPassword");
    // Target defaults to self.
    std::string target = conn.username;
    if (const Value* u = req.find("username"); u != nullptr) {
        if (!u->is<std::string>()) {
            throw CommandError("BadRequest", "\"username\" must be a string");
        }
        target = u->get<std::string>();
    }

    const bool isSelf = (target == conn.username);
    if (isSelf) {
        // Self-service requires proving knowledge of the current password.
        std::string oldPassword = requireString(req, "oldPassword");
        if (!server.users().verify(target, oldPassword)) {
            throw CommandError("AuthFailed", "current password is incorrect");
        }
    } else {
        // Resetting someone else's password is an admin action.
        if (!auth::rolesGrant(conn.roles, auth::Capability::Admin)) {
            throw CommandError("Forbidden", "admin role required to reset another user's password");
        }
    }
    if (!server.users().setPassword(target, newPassword)) {
        throw CommandError("BadRequest", "no such user");
    }
    // Force re-auth everywhere the old password was used.
    server.tokens().revokeAllForUser(target);
    server.auditLog("auth  password changed for: " + target);
    return okResponse();
}

// Setup-mode-only: create the first admin using the one-time bootstrap token.
Value cmdCreateUserBootstrap(Server& server, const Document& req, ConnectionAuth& conn) {
    std::string presented = requireString(req, "bootstrapToken");
    if (!server.checkBootstrapToken(presented)) {
        throw CommandError("AuthFailed", "invalid bootstrap token");
    }
    std::string username = requireString(req, "username");
    std::string password = requireString(req, "password");
    std::vector<auth::Role> roles = requireRoles(req);
    if (!auth::rolesContainAdmin(roles)) {
        throw CommandError("BadRequest", "the first user created in setup mode must be an admin");
    }
    server.users().createUser(username, password, roles);
    server.endSetupMode();
    server.auditLog("auth  first admin created via bootstrap: " + username);

    // Log them straight in so the client is authenticated after bootstrap.
    std::int64_t now = nowMs();
    crypto::Token token =
        server.tokens().issue(username, roles, server.config().tokenTtlSeconds, now);
    conn.authenticated = true;
    conn.username = username;
    conn.roles = roles;
    conn.expiresAtMs = now + server.config().tokenTtlSeconds * 1000;
    conn.tokenHash = token.hashHex;
    return okResponse(Document{{"token", Value(token.raw)},
                               {"expiresInSec", Value(server.config().tokenTtlSeconds)},
                               {"username", Value(username)},
                               {"roles", rolesToArray(roles)}});
}

// ---- exception -> error code translation (the single place) ---------------

Value translateActiveException(const std::string& cmd) {
    try {
        throw;
    } catch (const CommandError& e) {
        return errorResponse(e.code(), e.what());
    } catch (const btree::DuplicateKeyError& e) {
        return errorResponse("DuplicateKey", e.what());
    } catch (const btree::KeyTooLong& e) {
        return errorResponse("TooLarge", e.what());
    } catch (const BsonParseError& e) {
        return errorResponse("CorruptData", e.what());
    } catch (const JsonParseError& e) {
        return errorResponse("ParseError", e.what());
    } catch (const btree::RebuildRequired& e) {
        return errorResponse("CorruptData", e.what());
    } catch (const query::QueryError& e) {
        return errorResponse("BadRequest", e.what());
    } catch (const auth::AuthStoreError& e) {
        return errorResponse("BadRequest", e.what());
    } catch (const store::StoreError& e) {
        return errorResponse("BadRequest", e.what());
    } catch (const TypeError& e) {
        return errorResponse("BadRequest", e.what());
    } catch (const std::exception& e) {
        return errorResponse("Internal", "command \"" + cmd + "\" failed: " + e.what());
    } catch (...) {
        return errorResponse("Internal", "command \"" + cmd + "\" failed: unknown error");
    }
}

} // namespace

Value dispatchCommand(Server& server, const Value& request, const net::Stream& peer,
                      ConnectionAuth& conn) {
    std::string cmd;
    try {
        if (!request.is<Document>()) {
            throw CommandError("BadRequest", "request must be a BSON document");
        }
        const Document& req = request.asDocument();
        cmd = requireString(req, "cmd");
        server.stats().countOp(cmd);

        // ── Phase 1: commands allowed in ANY state (incl. UNAUTHENTICATED) ──
        if (cmd == "ping") {
            return okResponse();
        }
        if (cmd == "serverStatus") {
            return cmdServerStatus(server, conn);
        }
        if (cmd == "authenticate") {
            return cmdAuthenticate(server, req, conn);
        }
        if (cmd == "authenticateToken") {
            return cmdAuthenticateToken(server, req, conn);
        }

        // ── Phase 2: the auth gate (skipped only in --no-auth mode) ─────────
        if (server.authActive()) {
            if (server.inSetupMode()) {
                // Before the first admin exists, the ONLY privileged action is
                // bootstrapping that admin with the one-time token.
                if (cmd == "createUser") {
                    return cmdCreateUserBootstrap(server, req, conn);
                }
                throw CommandError("AuthRequired",
                                   "server is in setup mode; create the first admin with the "
                                   "bootstrap token");
            }
            if (!conn.authenticated) {
                throw CommandError("AuthRequired", "authentication required");
            }
            // Re-validate the backing token every command: catches expiry and
            // server-side revocation (drop/disable/password change/logout).
            auth::TokenInfo info;
            switch (server.tokens().resolveByHash(conn.tokenHash, nowMs(), info)) {
            case auth::TokenStore::Lookup::Expired:
                conn.authenticated = false;
                throw CommandError("TokenExpired", "session token expired; re-authenticate");
            case auth::TokenStore::Lookup::NotFound:
                conn.authenticated = false;
                throw CommandError("AuthRequired", "session ended; re-authenticate");
            case auth::TokenStore::Lookup::Valid:
                conn.roles = info.roles; // pick up any role changes
                break;
            }
            // Capability check: the connection's roles must grant what the
            // command needs. changePassword/logout are None and self-gated.
            if (!auth::rolesGrant(conn.roles, capabilityFor(cmd))) {
                throw CommandError("Forbidden", "insufficient privileges for \"" + cmd + "\"");
            }
        }

        // ── Phase 3: authenticated commands ─────────────────────────────────
        if (cmd == "logout") {
            return cmdLogout(server, conn);
        }
        // User management is meaningless (and the user store is absent) without
        // auth enabled.
        if (!server.authActive() && (cmd == "createUser" || cmd == "dropUser" ||
                                     cmd == "listUsers" || cmd == "changePassword")) {
            throw CommandError("BadRequest", "user management is disabled (--no-auth)");
        }
        if (cmd == "createUser") {
            return cmdCreateUser(server, req);
        }
        if (cmd == "dropUser") {
            return cmdDropUser(server, req, conn);
        }
        if (cmd == "listUsers") {
            return cmdListUsers(server);
        }
        if (cmd == "changePassword") {
            return cmdChangePassword(server, req, conn);
        }
        if (cmd == "listCollections") {
            Array names;
            for (const std::string& n : server.database().listCollections()) {
                names.push_back(Value(n));
            }
            return okResponse(Document{{"collections", Value(std::move(names))}});
        }
        if (cmd == "createCollection") {
            bool created = server.database().createCollection(requireString(req, "coll"));
            return okResponse(Document{{"created", Value(created)}});
        }
        if (cmd == "dbStats") {
            Array collections;
            for (const std::string& name : server.database().listCollections()) {
                query::IndexedCollection& coll = server.database().collection(name);
                Array indexes;
                for (const std::string& f : coll.listIndexes()) {
                    indexes.push_back(Value(f));
                }
                namespace fs = std::filesystem;
                uint64_t bytes = 0;
                fs::path dir(server.database().dir());
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(dir, ec)) {
                    std::string file = entry.path().filename().string();
                    if (file == name + ".log" || file == name + ".meta.json" ||
                        (entry.path().extension() == ".idx" && file.rfind(name + ".", 0) == 0)) {
                        bytes += entry.file_size(ec);
                    }
                }
                collections.push_back(
                    Value(Document{{"name", Value(name)},
                                   {"count", Value(static_cast<int64_t>(coll.count()))},
                                   {"fileSizeBytes", Value(static_cast<int64_t>(bytes))},
                                   {"indexes", Value(std::move(indexes))}}));
            }
            return okResponse(Document{{"collections", Value(std::move(collections))}});
        }
        if (cmd == "dropCollection") {
            bool dropped = server.database().dropCollection(requireString(req, "coll"));
            return okResponse(Document{{"dropped", Value(dropped)}});
        }
        if (cmd == "insert") {
            return cmdInsert(server, req);
        }
        if (cmd == "find") {
            return cmdFind(server, req);
        }
        if (cmd == "deleteMany") {
            return cmdDeleteMany(server, req);
        }
        if (cmd == "updateOne") {
            return cmdUpdateOne(server, req);
        }
        if (cmd == "createIndex") {
            return cmdCreateIndex(server, req);
        }
        if (cmd == "dropIndex") {
            collOf(server, req).dropIndex(requireString(req, "field"));
            return okResponse();
        }
        if (cmd == "listIndexes") {
            Array indexes;
            for (const std::string& f : collOf(server, req).listIndexes()) {
                indexes.push_back(Value(f));
            }
            return okResponse(Document{{"indexes", Value(std::move(indexes))}});
        }
        if (cmd == "explain") {
            return cmdExplain(server, req);
        }
        if (cmd == "compact") {
            query::IndexedCollection& coll = collOf(server, req);
            std::size_t before = coll.count();
            coll.compact();
            return okResponse(Document{
                {"stats", Value(Document{{"documents", Value(static_cast<int64_t>(before))}})}});
        }
        if (cmd == "shutdown") {
            if (!peer.isLoopbackPeer()) {
                throw CommandError("BadRequest", "shutdown is only accepted from loopback");
            }
            server.requestStop();
            return okResponse();
        }
        throw CommandError("UnknownCommand", "unknown command \"" + cmd + "\"");
    } catch (...) {
        return translateActiveException(cmd);
    }
}

} // namespace bisondb::server

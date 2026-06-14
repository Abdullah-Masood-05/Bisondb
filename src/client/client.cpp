#include "client/client.hpp"

#include "server/protocol.hpp"

namespace bisondb::client {

namespace {

Document docArg(const std::string& cmd, const std::string& coll) {
    return Document{{"cmd", Value(cmd)}, {"coll", Value(coll)}};
}

bool isAuthCode(const std::string& code) {
    return code == "AuthFailed" || code == "AuthRequired" || code == "TokenExpired" ||
           code == "Forbidden";
}

std::vector<std::string> parseRoles(const Document& payload) {
    std::vector<std::string> roles;
    if (const Value* r = payload.find("roles"); r != nullptr && r->is<Array>()) {
        for (const Value& v : r->asArray()) {
            if (v.is<std::string>()) {
                roles.push_back(v.get<std::string>());
            }
        }
    }
    return roles;
}

} // namespace

BisonClient BisonClient::establish(const std::string& host, uint16_t port, const TlsOptions& tls,
                                   int timeoutMs) {
    net::TcpSocket socket = net::connectTcp(host, port, timeoutMs);
    if (!tls.enabled) {
        return BisonClient(std::make_unique<net::TcpStream>(std::move(socket)));
    }
    net::TlsClientOptions o;
    o.verify = tls.verify;
    o.caFile = tls.caFile;
    o.pinSha256Hex = tls.pinSha256;
    o.hostname = tls.hostname.empty() ? host : tls.hostname;
    auto ctx = net::TlsContext::client(o);
    try {
        BisonClient c(
            std::make_unique<net::TlsStream>(net::TlsStream::connect(ctx, std::move(socket))));
        c.tls_ = true;
        c.tlsVerified_ = (tls.verify != net::TlsVerify::Insecure);
        return c;
    } catch (const net::TlsError& e) {
        // A plaintext server looks like a broken handshake — guide the user.
        if (e.kind() == net::TlsError::Kind::Handshake) {
            throw net::TlsError(e.kind(), std::string(e.what()) +
                                              " — is the server actually using TLS? If it is "
                                              "plaintext, connect without --tls.");
        }
        throw;
    }
}

BisonClient BisonClient::connect(const std::string& host, uint16_t port, int timeoutMs) {
    return establish(host, port, TlsOptions{}, timeoutMs);
}

BisonClient BisonClient::connect(const std::string& host, uint16_t port, const TlsOptions& tls,
                                 int timeoutMs) {
    return establish(host, port, tls, timeoutMs);
}

BisonClient BisonClient::connect(const std::string& host, uint16_t port, const Credentials& creds,
                                 int timeoutMs) {
    return connect(host, port, TlsOptions{}, creds, timeoutMs);
}

BisonClient BisonClient::connect(const std::string& host, uint16_t port, const TlsOptions& tls,
                                 const Credentials& creds, int timeoutMs) {
    BisonClient c = establish(host, port, tls, timeoutMs);
    if (creds.usesToken()) {
        c.authenticateToken(creds.token);
    } else {
        c.authenticate(creds.username, creds.password);
    }
    return c;
}

Value BisonClient::sendOnce(const Value& request) {
    server::writeFrame(*stream_, request);
    std::optional<Value> response = server::readFrame(*stream_);
    if (!response.has_value()) {
        throw net::NetError(net::NetError::Kind::Closed, "server closed the connection");
    }
    const Document& doc = response->asDocument();
    const Value* ok = doc.find("ok");
    if (ok != nullptr && ok->is<bool>() && ok->get<bool>()) {
        return std::move(*response);
    }
    std::string code = "Internal";
    std::string message = "malformed error response";
    if (const Value* err = doc.find("error"); err != nullptr && err->is<Document>()) {
        if (const Value* c = err->asDocument().find("code"); c && c->is<std::string>()) {
            code = c->get<std::string>();
        }
        if (const Value* m = err->asDocument().find("message"); m && m->is<std::string>()) {
            message = m->get<std::string>();
        }
    }
    throw ServerError(code, message);
}

Value BisonClient::command(Value request) {
    try {
        return sendOnce(request);
    } catch (const ServerError& e) {
        // One transparent re-auth attempt on token expiry, if we can.
        if (e.code() == "TokenExpired" && !username_.empty() && !password_.empty()) {
            authenticate(username_, password_); // refreshes token_, may throw AuthError
            return sendOnce(request);           // retry once
        }
        if (isAuthCode(e.code())) {
            throw AuthError(e.code(), e.what());
        }
        throw;
    }
}

std::vector<std::string> BisonClient::authenticate(const std::string& username,
                                                   const std::string& password) {
    Value resp;
    try {
        resp = sendOnce(Value(Document{{"cmd", Value("authenticate")},
                                       {"username", Value(username)},
                                       {"password", Value(password)}}));
    } catch (const ServerError& e) {
        throw AuthError(e.code(), e.what());
    }
    const Document& d = resp.asDocument();
    token_ = d.find("token") ? d.find("token")->get<std::string>() : "";
    roles_ = parseRoles(d);
    username_ = username;
    password_ = password;
    authenticated_ = true;
    return roles_;
}

std::vector<std::string> BisonClient::authenticateToken(const std::string& token) {
    Value resp;
    try {
        resp =
            sendOnce(Value(Document{{"cmd", Value("authenticateToken")}, {"token", Value(token)}}));
    } catch (const ServerError& e) {
        throw AuthError(e.code(), e.what());
    }
    const Document& d = resp.asDocument();
    token_ = token;
    roles_ = parseRoles(d);
    if (const Value* u = d.find("username"); u != nullptr && u->is<std::string>()) {
        username_ = u->get<std::string>();
    }
    password_.clear(); // token-only session: no transparent refresh possible
    authenticated_ = true;
    return roles_;
}

void BisonClient::logout() {
    try {
        sendOnce(Value(Document{{"cmd", Value("logout")}}));
    } catch (const ServerError&) {
        // best-effort; clear local state regardless
    }
    authenticated_ = false;
    username_.clear();
    password_.clear();
    token_.clear();
    roles_.clear();
}

std::vector<std::string> BisonClient::bootstrapAdmin(const std::string& bootstrapToken,
                                                     const std::string& username,
                                                     const std::string& password) {
    Value resp;
    try {
        Array roles{Value("admin")};
        resp = sendOnce(Value(Document{{"cmd", Value("createUser")},
                                       {"bootstrapToken", Value(bootstrapToken)},
                                       {"username", Value(username)},
                                       {"password", Value(password)},
                                       {"roles", Value(std::move(roles))}}));
    } catch (const ServerError& e) {
        throw AuthError(e.code(), e.what());
    }
    const Document& d = resp.asDocument();
    token_ = d.find("token") ? d.find("token")->get<std::string>() : "";
    roles_ = parseRoles(d);
    username_ = username;
    password_ = password;
    authenticated_ = true;
    return roles_;
}

void BisonClient::createUser(const std::string& username, const std::string& password,
                             const std::vector<std::string>& roles) {
    Array roleArr;
    for (const std::string& r : roles) {
        roleArr.push_back(Value(r));
    }
    command(Value(Document{{"cmd", Value("createUser")},
                           {"username", Value(username)},
                           {"password", Value(password)},
                           {"roles", Value(std::move(roleArr))}}));
}

bool BisonClient::dropUser(const std::string& username) {
    Value resp =
        command(Value(Document{{"cmd", Value("dropUser")}, {"username", Value(username)}}));
    const Value* dropped = resp.asDocument().find("dropped");
    return dropped != nullptr && dropped->is<bool>() && dropped->get<bool>();
}

std::vector<UserInfo> BisonClient::listUsers() {
    Value resp = command(Value(Document{{"cmd", Value("listUsers")}}));
    std::vector<UserInfo> out;
    if (const Value* users = resp.asDocument().find("users"); users && users->is<Array>()) {
        for (const Value& v : users->asArray()) {
            const Document& u = v.asDocument();
            UserInfo info;
            if (const Value* n = u.find("username"))
                info.username = n->get<std::string>();
            info.roles = parseRoles(u);
            if (const Value* dis = u.find("disabled"); dis && dis->is<bool>()) {
                info.disabled = dis->get<bool>();
            }
            out.push_back(std::move(info));
        }
    }
    return out;
}

void BisonClient::changePassword(const std::string& newPassword, const std::string& oldPassword,
                                 const std::string& targetUser) {
    Document req{{"cmd", Value("changePassword")}, {"newPassword", Value(newPassword)}};
    if (!targetUser.empty()) {
        req.append("username", Value(targetUser));
    }
    if (!oldPassword.empty()) {
        req.append("oldPassword", Value(oldPassword));
    }
    command(Value(std::move(req)));
}

void BisonClient::ping() {
    command(Value(Document{{"cmd", Value("ping")}}));
}

Value BisonClient::serverStatus() {
    return command(Value(Document{{"cmd", Value("serverStatus")}}));
}

std::vector<std::string> BisonClient::listCollections() {
    Value resp = command(Value(Document{{"cmd", Value("listCollections")}}));
    std::vector<std::string> out;
    for (const Value& v : resp.asDocument().find("collections")->asArray()) {
        out.push_back(v.get<std::string>());
    }
    return out;
}

bool BisonClient::createCollection(const std::string& coll) {
    Value resp = command(Value(docArg("createCollection", coll)));
    return resp.asDocument().find("created")->get<bool>();
}

Value BisonClient::dbStats() {
    return command(Value(Document{{"cmd", Value("dbStats")}}));
}

bool BisonClient::dropCollection(const std::string& coll) {
    Value resp = command(Value(docArg("dropCollection", coll)));
    return resp.asDocument().find("dropped")->get<bool>();
}

std::vector<ObjectId> BisonClient::insert(const std::string& coll,
                                          const std::vector<Value>& documents) {
    Document req = docArg("insert", coll);
    req.append("documents", Value(Array(documents.begin(), documents.end())));
    Value resp = command(Value(std::move(req)));
    std::vector<ObjectId> ids;
    for (const Value& v : resp.asDocument().find("insertedIds")->asArray()) {
        ids.push_back(v.get<ObjectId>());
    }
    return ids;
}

std::vector<Value> BisonClient::find(const std::string& coll, const Value& filter,
                                     const FindOptions& options) {
    std::vector<Value> out;
    std::size_t skip = options.skip;
    while (true) {
        Document req = docArg("find", coll);
        req.append("filter", filter);
        if (options.limit != 0) {
            req.append("limit", Value(static_cast<int64_t>(options.limit - out.size())));
        }
        req.append("skip", Value(static_cast<int64_t>(skip)));
        Value resp = command(Value(std::move(req)));
        const Document& payload = resp.asDocument();
        for (const Value& doc : payload.find("documents")->asArray()) {
            out.push_back(doc);
        }
        const Value* truncated = payload.find("truncated");
        bool more = truncated != nullptr && truncated->is<bool>() && truncated->get<bool>();
        if (!more || options.singleBatch) {
            return out;
        }
        if (options.limit != 0 && out.size() >= options.limit) {
            return out;
        }
        skip = static_cast<std::size_t>(payload.find("skipNext")->get<int64_t>());
    }
}

std::size_t BisonClient::deleteMany(const std::string& coll, const Value& filter) {
    Document req = docArg("deleteMany", coll);
    req.append("filter", filter);
    Value resp = command(Value(std::move(req)));
    return static_cast<std::size_t>(resp.asDocument().find("deletedCount")->get<int64_t>());
}

bool BisonClient::updateOne(const std::string& coll, const Value& filter, const Value& update) {
    Document req = docArg("updateOne", coll);
    req.append("filter", filter);
    req.append("update", update);
    Value resp = command(Value(std::move(req)));
    return resp.asDocument().find("matched")->get<bool>();
}

int64_t BisonClient::createIndex(const std::string& coll, const std::string& field) {
    Document req = docArg("createIndex", coll);
    req.append("field", Value(field));
    Value resp = command(Value(std::move(req)));
    return resp.asDocument().find("docsIndexed")->get<int64_t>();
}

void BisonClient::dropIndex(const std::string& coll, const std::string& field) {
    Document req = docArg("dropIndex", coll);
    req.append("field", Value(field));
    command(Value(std::move(req)));
}

std::vector<std::string> BisonClient::listIndexes(const std::string& coll) {
    Value resp = command(Value(docArg("listIndexes", coll)));
    std::vector<std::string> out;
    for (const Value& v : resp.asDocument().find("indexes")->asArray()) {
        out.push_back(v.get<std::string>());
    }
    return out;
}

Value BisonClient::explain(const std::string& coll, const Value& filter, std::size_t limit) {
    Document req = docArg("explain", coll);
    req.append("filter", filter);
    if (limit != 0) {
        req.append("limit", Value(static_cast<int64_t>(limit)));
    }
    Value resp = command(Value(std::move(req)));
    return *resp.asDocument().find("plan");
}

void BisonClient::compact(const std::string& coll) {
    command(Value(docArg("compact", coll)));
}

void BisonClient::shutdownServer() {
    command(Value(Document{{"cmd", Value("shutdown")}}));
}

} // namespace bisondb::client

#include "core/auth/user_store.hpp"

#include "core/bson_decoder.hpp"
#include "core/bson_encoder.hpp"
#include "core/value.hpp"

#include <array>
#include <chrono>
#include <cstring>

namespace bisondb::auth {
namespace {

constexpr std::uint8_t kRecPut = 1;
constexpr std::uint8_t kRecDel = 2;
// A single user record is tiny; anything larger signals corruption.
constexpr std::uint32_t kMaxRecordLen = 1u << 20; // 1 MiB

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void putU32le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

} // namespace

bool isValidUsername(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    auto isFirst = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_';
    };
    auto isRest = [&](char c) { return isFirst(c) || c == '.' || c == '-'; };
    if (!isFirst(name[0])) {
        return false;
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (!isRest(name[i])) {
            return false;
        }
    }
    return true;
}

UserStore::UserStore(std::string dbdir, crypto::KdfParams kdf)
    : path_(std::move(dbdir)), kdf_(kdf) {
    if (!path_.empty() && path_.back() != '/' && path_.back() != '\\') {
        path_ += '/';
    }
    path_ += "__auth.bsd";
    // A constant-cost dummy used to equalize timing for unknown users.
    dummyHash_ = crypto::hashPassword("bisondb-dummy-password", kdf_);
    replay();
    openFile();
}

UserStore::~UserStore() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

void UserStore::openFile() {
    // Append mode; the file is created if absent.
    file_ = std::fopen(path_.c_str(), "ab");
    if (file_ == nullptr) {
        throw AuthStoreError("cannot open user store: " + path_);
    }
}

void UserStore::replay() {
    std::FILE* f = std::fopen(path_.c_str(), "rb");
    if (f == nullptr) {
        return; // first run — no file yet
    }
    while (true) {
        int typeByte = std::fgetc(f);
        if (typeByte == EOF) {
            break;
        }
        std::array<std::uint8_t, 4> lenBuf{};
        if (std::fread(lenBuf.data(), 1, 4, f) != 4) {
            break; // torn tail
        }
        std::uint32_t len = static_cast<std::uint32_t>(lenBuf[0]) |
                            (static_cast<std::uint32_t>(lenBuf[1]) << 8) |
                            (static_cast<std::uint32_t>(lenBuf[2]) << 16) |
                            (static_cast<std::uint32_t>(lenBuf[3]) << 24);
        if (len > kMaxRecordLen) {
            break; // corrupt
        }
        std::vector<std::uint8_t> payload(len);
        if (len > 0 && std::fread(payload.data(), 1, len, f) != len) {
            break; // torn tail
        }
        if (typeByte == kRecPut) {
            try {
                UserRecord rec = fromDocument(decodeDocument(payload));
                users_[rec.username] = std::move(rec);
            } catch (const std::exception&) {
                // Skip an undecodable record rather than abort the whole store.
            }
        } else if (typeByte == kRecDel) {
            std::string username(reinterpret_cast<const char*>(payload.data()), payload.size());
            users_.erase(username);
        } else {
            break; // unknown record type — stop
        }
    }
    std::fclose(f);
}

Value UserStore::toDocument(const UserRecord& rec) const {
    Document d;
    d.append("username", rec.username);
    d.append("pwHash", rec.password.hashHex);
    d.append("pwSalt", rec.password.saltHex);
    d.append("kdfParams", rec.password.params);
    Array roles;
    for (Role r : rec.roles) {
        roles.emplace_back(std::string(roleName(r)));
    }
    d.append("roles", std::move(roles));
    d.append("createdAt", DateTime{rec.createdAtMs});
    d.append("disabled", rec.disabled);
    return Value(std::move(d));
}

UserRecord UserStore::fromDocument(const Value& doc) const {
    const Document& d = doc.asDocument();
    UserRecord rec;
    if (const Value* v = d.find("username")) {
        rec.username = v->get<std::string>();
    } else {
        throw AuthStoreError("user record missing username");
    }
    if (const Value* v = d.find("pwHash"))
        rec.password.hashHex = v->get<std::string>();
    if (const Value* v = d.find("pwSalt"))
        rec.password.saltHex = v->get<std::string>();
    if (const Value* v = d.find("kdfParams"))
        rec.password.params = v->get<std::string>();
    if (const Value* v = d.find("roles")) {
        for (const Value& r : v->asArray()) {
            if (auto parsed = parseRole(r.get<std::string>())) {
                rec.roles.push_back(*parsed);
            }
            // Unknown roles are silently dropped (forward-compat).
        }
    }
    if (const Value* v = d.find("createdAt")) {
        if (v->is<DateTime>())
            rec.createdAtMs = v->get<DateTime>().msSinceEpoch;
    }
    if (const Value* v = d.find("disabled")) {
        if (v->is<bool>())
            rec.disabled = v->get<bool>();
    }
    return rec;
}

void UserStore::appendPut(const UserRecord& rec) {
    std::vector<std::uint8_t> bson = encodeDocument(toDocument(rec));
    std::vector<std::uint8_t> frame;
    frame.reserve(5 + bson.size());
    frame.push_back(kRecPut);
    putU32le(frame, static_cast<std::uint32_t>(bson.size()));
    frame.insert(frame.end(), bson.begin(), bson.end());
    if (std::fwrite(frame.data(), 1, frame.size(), file_) != frame.size()) {
        throw AuthStoreError("failed to write user record");
    }
    std::fflush(file_);
}

void UserStore::appendDelete(const std::string& username) {
    std::vector<std::uint8_t> frame;
    frame.reserve(5 + username.size());
    frame.push_back(kRecDel);
    putU32le(frame, static_cast<std::uint32_t>(username.size()));
    frame.insert(frame.end(), username.begin(), username.end());
    if (std::fwrite(frame.data(), 1, frame.size(), file_) != frame.size()) {
        throw AuthStoreError("failed to write user tombstone");
    }
    std::fflush(file_);
}

bool UserStore::empty() {
    std::lock_guard lock(mutex_);
    return users_.empty();
}

std::size_t UserStore::userCount() {
    std::lock_guard lock(mutex_);
    return users_.size();
}

std::size_t UserStore::adminCount() {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& [name, rec] : users_) {
        if (!rec.disabled && rolesContainAdmin(rec.roles)) {
            ++n;
        }
    }
    return n;
}

bool UserStore::exists(const std::string& username) {
    std::lock_guard lock(mutex_);
    return users_.count(username) != 0;
}

std::optional<UserRecord> UserStore::get(const std::string& username) {
    std::lock_guard lock(mutex_);
    auto it = users_.find(username);
    if (it == users_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<UserRecord> UserStore::list() {
    std::lock_guard lock(mutex_);
    std::vector<UserRecord> out;
    out.reserve(users_.size());
    for (const auto& [name, rec] : users_) {
        out.push_back(rec);
    }
    return out;
}

void UserStore::createUser(const std::string& username, std::string_view password,
                           const std::vector<Role>& roles) {
    if (!isValidUsername(username)) {
        throw AuthStoreError("invalid username");
    }
    if (password.empty()) {
        throw AuthStoreError("password must not be empty");
    }
    if (roles.empty()) {
        throw AuthStoreError("at least one role is required");
    }
    std::lock_guard lock(mutex_);
    if (users_.count(username) != 0) {
        throw AuthStoreError("user already exists");
    }
    UserRecord rec;
    rec.username = username;
    rec.password = crypto::hashPassword(password, kdf_);
    rec.roles = roles;
    rec.createdAtMs = nowMs();
    rec.disabled = false;
    appendPut(rec);
    users_[username] = std::move(rec);
}

bool UserStore::dropUser(const std::string& username) {
    std::lock_guard lock(mutex_);
    auto it = users_.find(username);
    if (it == users_.end()) {
        return false;
    }
    // Anti-lockout: never drop the last enabled admin.
    if (!it->second.disabled && rolesContainAdmin(it->second.roles)) {
        std::size_t enabledAdmins = 0;
        for (const auto& [name, rec] : users_) {
            if (!rec.disabled && rolesContainAdmin(rec.roles)) {
                ++enabledAdmins;
            }
        }
        if (enabledAdmins <= 1) {
            throw AuthStoreError("cannot drop the last admin");
        }
    }
    appendDelete(username);
    users_.erase(it);
    return true;
}

bool UserStore::setDisabled(const std::string& username, bool disabled) {
    std::lock_guard lock(mutex_);
    auto it = users_.find(username);
    if (it == users_.end()) {
        return false;
    }
    if (disabled && !it->second.disabled && rolesContainAdmin(it->second.roles)) {
        std::size_t enabledAdmins = 0;
        for (const auto& [name, rec] : users_) {
            if (!rec.disabled && rolesContainAdmin(rec.roles)) {
                ++enabledAdmins;
            }
        }
        if (enabledAdmins <= 1) {
            throw AuthStoreError("cannot disable the last admin");
        }
    }
    UserRecord rec = it->second;
    rec.disabled = disabled;
    appendPut(rec);
    it->second = std::move(rec);
    return true;
}

bool UserStore::setPassword(const std::string& username, std::string_view newPassword) {
    if (newPassword.empty()) {
        throw AuthStoreError("password must not be empty");
    }
    std::lock_guard lock(mutex_);
    auto it = users_.find(username);
    if (it == users_.end()) {
        return false;
    }
    UserRecord rec = it->second;
    rec.password = crypto::hashPassword(newPassword, kdf_);
    appendPut(rec);
    it->second = std::move(rec);
    return true;
}

std::optional<UserRecord> UserStore::verify(const std::string& username,
                                            std::string_view password) {
    std::lock_guard lock(mutex_);
    auto it = users_.find(username);
    if (it == users_.end()) {
        // Spend comparable time so callers can't distinguish unknown-user from
        // wrong-password by timing.
        (void)crypto::verifyPassword(password, dummyHash_);
        return std::nullopt;
    }
    bool ok = crypto::verifyPassword(password, it->second.password);
    if (!ok || it->second.disabled) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace bisondb::auth

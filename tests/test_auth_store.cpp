#include "core/auth/token_store.hpp"
#include "core/auth/user_store.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <set>
#include <string>

using namespace bisondb;
using namespace bisondb::auth;
namespace fs = std::filesystem;

namespace {

// Cheap KDF so the suite stays fast; production uses the ~19 MiB default.
crypto::KdfParams fastKdf() {
    crypto::KdfParams p;
    p.memoryKiB = 64;
    p.passes = 1;
    p.lanes = 1;
    return p;
}

struct TempDir {
    fs::path dir;
    explicit TempDir(const std::string& name) {
        dir = fs::temp_directory_path() / ("bisondb_auth_" + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~TempDir() { fs::remove_all(dir); }
    std::string str() const { return dir.string(); }
};

} // namespace

TEST_CASE("username validation", "[auth]") {
    REQUIRE(isValidUsername("admin"));
    REQUIRE(isValidUsername("_root"));
    REQUIRE(isValidUsername("a.b-c_9"));
    REQUIRE_FALSE(isValidUsername(""));
    REQUIRE_FALSE(isValidUsername(".leadingdot"));
    REQUIRE_FALSE(isValidUsername("has space"));
    REQUIRE_FALSE(isValidUsername("tab\tname"));
    REQUIRE_FALSE(isValidUsername(std::string(65, 'a')));
}

TEST_CASE("user store: create, list, drop, duplicate", "[auth]") {
    TempDir td("crud");
    UserStore store(td.str(), fastKdf());

    REQUIRE(store.empty());
    store.createUser("alice", "s3cret-pass", {Role::Admin});
    store.createUser("bob", "another-pass", {Role::Read});

    REQUIRE_FALSE(store.empty());
    REQUIRE(store.userCount() == 2);
    REQUIRE(store.exists("alice"));
    REQUIRE_FALSE(store.exists("carol"));
    REQUIRE(store.list().size() == 2);

    // Duplicate rejected.
    REQUIRE_THROWS_AS(store.createUser("alice", "x", {Role::Read}), AuthStoreError);
    // Empty password / no roles rejected.
    REQUIRE_THROWS_AS(store.createUser("dave", "", {Role::Read}), AuthStoreError);
    REQUIRE_THROWS_AS(store.createUser("dave", "pw", {}), AuthStoreError);
    // Invalid username rejected.
    REQUIRE_THROWS_AS(store.createUser("bad name", "pw", {Role::Read}), AuthStoreError);

    REQUIRE(store.dropUser("bob"));
    REQUIRE_FALSE(store.dropUser("bob")); // already gone
    REQUIRE(store.userCount() == 1);
}

TEST_CASE("user store: verify, wrong password, no user enumeration", "[auth]") {
    TempDir td("verify");
    UserStore store(td.str(), fastKdf());
    store.createUser("alice", "correct-password", {Role::ReadWrite});

    REQUIRE(store.verify("alice", "correct-password").has_value());
    // Wrong password and unknown user are both nullopt — indistinguishable.
    REQUIRE_FALSE(store.verify("alice", "wrong-password").has_value());
    REQUIRE_FALSE(store.verify("nonexistent", "any-password").has_value());

    auto rec = store.verify("alice", "correct-password");
    REQUIRE(rec->roles.size() == 1);
    REQUIRE(rec->roles[0] == Role::ReadWrite);
}

TEST_CASE("user store: disabled user cannot verify", "[auth]") {
    TempDir td("disabled");
    UserStore store(td.str(), fastKdf());
    store.createUser("admin", "adminpw", {Role::Admin});
    store.createUser("bob", "bobpw", {Role::Read});

    REQUIRE(store.verify("bob", "bobpw").has_value());
    REQUIRE(store.setDisabled("bob", true));
    REQUIRE_FALSE(store.verify("bob", "bobpw").has_value());
    REQUIRE(store.setDisabled("bob", false));
    REQUIRE(store.verify("bob", "bobpw").has_value());
}

TEST_CASE("user store: password change (admin reset)", "[auth]") {
    TempDir td("passwd");
    UserStore store(td.str(), fastKdf());
    store.createUser("bob", "oldpw", {Role::Read});

    REQUIRE(store.verify("bob", "oldpw").has_value());
    REQUIRE(store.setPassword("bob", "newpw"));
    REQUIRE_FALSE(store.verify("bob", "oldpw").has_value());
    REQUIRE(store.verify("bob", "newpw").has_value());
    REQUIRE_FALSE(store.setPassword("ghost", "x")); // absent
    REQUIRE_THROWS_AS(store.setPassword("bob", ""), AuthStoreError);
}

TEST_CASE("user store: anti-lockout protects the last admin", "[auth]") {
    TempDir td("lockout");
    UserStore store(td.str(), fastKdf());
    store.createUser("root", "rootpw", {Role::Admin});

    REQUIRE_THROWS_AS(store.dropUser("root"), AuthStoreError);
    REQUIRE_THROWS_AS(store.setDisabled("root", true), AuthStoreError);

    // With a second admin, the first can be dropped.
    store.createUser("root2", "root2pw", {Role::Admin});
    REQUIRE(store.adminCount() == 2);
    REQUIRE(store.dropUser("root"));
    REQUIRE_THROWS_AS(store.dropUser("root2"), AuthStoreError); // now the last again
}

TEST_CASE("user store: persists across reopen (replay)", "[auth]") {
    TempDir td("persist");
    {
        UserStore store(td.str(), fastKdf());
        store.createUser("alice", "alicepw", {Role::Admin});
        store.createUser("bob", "bobpw", {Role::Read});
        store.setDisabled("bob", true);
        store.setPassword("alice", "alicepw2");
        store.createUser("carol", "carolpw", {Role::ReadWrite});
        store.dropUser("carol");
    }
    // Reopen: state must reflect the full log (drops, disables, pw change).
    UserStore store(td.str(), fastKdf());
    REQUIRE(store.userCount() == 2);
    REQUIRE(store.exists("alice"));
    REQUIRE(store.exists("bob"));
    REQUIRE_FALSE(store.exists("carol"));
    REQUIRE(store.verify("alice", "alicepw2").has_value());
    REQUIRE_FALSE(store.verify("alice", "alicepw").has_value()); // old pw gone
    REQUIRE_FALSE(store.verify("bob", "bobpw").has_value());     // still disabled
    auto bob = store.get("bob");
    REQUIRE(bob.has_value());
    REQUIRE(bob->disabled);
}

TEST_CASE("token store: issue, resolve, revoke, uniqueness", "[auth]") {
    TokenStore store;
    const std::int64_t now = 1'000'000'000'000; // fixed clock
    std::vector<Role> roles{Role::ReadWrite};

    crypto::Token t = store.issue("alice", roles, /*ttl*/ 3600, now);
    REQUIRE(store.size() == 1);

    TokenInfo info;
    REQUIRE(store.resolve(t.raw, now + 1000, info) == TokenStore::Lookup::Valid);
    REQUIRE(info.username == "alice");
    REQUIRE(info.roles == roles);

    // Unknown token.
    REQUIRE(store.resolve("deadbeef", now, info) == TokenStore::Lookup::NotFound);

    // Revoke (logout).
    REQUIRE(store.revoke(t.raw));
    REQUIRE(store.resolve(t.raw, now, info) == TokenStore::Lookup::NotFound);
    REQUIRE_FALSE(store.revoke(t.raw));

    // Uniqueness across many issues.
    std::set<std::string> raws;
    for (int i = 0; i < 500; ++i) {
        auto x = store.issue("u", roles, 3600, now);
        REQUIRE(raws.insert(x.raw).second);
    }
}

TEST_CASE("token store: expiry and purge", "[auth]") {
    TokenStore store;
    const std::int64_t now = 5'000'000;
    crypto::Token t = store.issue("bob", {Role::Read}, /*ttl*/ 10, now);

    TokenInfo info;
    REQUIRE(store.resolve(t.raw, now + 9'000, info) == TokenStore::Lookup::Valid);
    // 10s later -> expired, and the resolve evicts it.
    REQUIRE(store.resolve(t.raw, now + 10'000, info) == TokenStore::Lookup::Expired);
    REQUIRE(store.resolve(t.raw, now + 10'000, info) == TokenStore::Lookup::NotFound);
    REQUIRE(store.size() == 0);
}

TEST_CASE("token store: revoke all for a user", "[auth]") {
    TokenStore store;
    const std::int64_t now = 1000;
    store.issue("alice", {Role::Admin}, 3600, now);
    store.issue("alice", {Role::Admin}, 3600, now);
    store.issue("bob", {Role::Read}, 3600, now);
    REQUIRE(store.size() == 3);

    REQUIRE(store.revokeAllForUser("alice") == 2);
    REQUIRE(store.size() == 1);
    REQUIRE(store.revokeAllForUser("ghost") == 0);
}

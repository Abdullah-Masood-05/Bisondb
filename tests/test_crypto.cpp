#include "core/crypto/crypto.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <set>
#include <string>

using namespace bisondb;
using Catch::Matchers::ContainsSubstring;

// Fast cost for tests — real servers use the much higher default in KdfParams.
// Argon2id at the production floor (~19 MiB) would make this suite slow.
static crypto::KdfParams fastParams() {
    crypto::KdfParams p;
    p.memoryKiB = 64;
    p.passes = 1;
    p.lanes = 1;
    return p;
}

TEST_CASE("password hash round-trips and rejects wrong password", "[crypto]") {
    auto h = crypto::hashPassword("correct horse battery staple", fastParams());

    REQUIRE_FALSE(h.hashHex.empty());
    REQUIRE(h.saltHex.size() == 32); // 16 bytes hex
    REQUIRE(h.hashHex.size() == 64); // 32 bytes hex
    REQUIRE_THAT(h.params, ContainsSubstring("argon2id$"));

    REQUIRE(crypto::verifyPassword("correct horse battery staple", h));
    REQUIRE_FALSE(crypto::verifyPassword("wrong password", h));
    REQUIRE_FALSE(crypto::verifyPassword("", h));
    // Near-miss (trailing space) must fail.
    REQUIRE_FALSE(crypto::verifyPassword("correct horse battery staple ", h));
}

TEST_CASE("same password hashes differently due to random salt", "[crypto]") {
    auto a = crypto::hashPassword("hunter2", fastParams());
    auto b = crypto::hashPassword("hunter2", fastParams());
    REQUIRE(a.saltHex != b.saltHex);
    REQUIRE(a.hashHex != b.hashHex);
    // ...yet both verify.
    REQUIRE(crypto::verifyPassword("hunter2", a));
    REQUIRE(crypto::verifyPassword("hunter2", b));
}

TEST_CASE("KdfParams serialize/parse round-trip", "[crypto]") {
    crypto::KdfParams p;
    p.memoryKiB = 19456;
    p.passes = 3;
    p.lanes = 2;
    auto s = p.serialize();
    REQUIRE(s == "argon2id$m=19456,t=3,p=2");

    auto q = crypto::KdfParams::parse(s);
    REQUIRE(q.memoryKiB == 19456);
    REQUIRE(q.passes == 3);
    REQUIRE(q.lanes == 2);
}

TEST_CASE("malformed stored hash fails closed, never throws", "[crypto]") {
    crypto::PasswordHash bad;
    bad.params = "argon2id$m=64,t=1,p=1";
    bad.saltHex = "not-hex";
    bad.hashHex = "deadbeef";
    REQUIRE_FALSE(crypto::verifyPassword("anything", bad));

    crypto::PasswordHash badParams;
    badParams.params = "scrypt$n=1024"; // unsupported id
    badParams.saltHex = "00112233445566778899aabbccddeeff";
    badParams.hashHex = std::string(64, 'a');
    REQUIRE_FALSE(crypto::verifyPassword("anything", badParams));
}

TEST_CASE("token generation: hash matches and tokens are unique", "[crypto]") {
    auto t = crypto::generateToken();
    REQUIRE(t.raw.size() == 64);     // 32 bytes hex
    REQUIRE(t.hashHex.size() == 64); // BLAKE2b-256 hex
    REQUIRE(t.raw != t.hashHex);     // raw is not its own hash
    REQUIRE(crypto::hashToken(t.raw) == t.hashHex);

    // CSPRNG uniqueness: 1000 tokens, no collisions.
    std::set<std::string> raws;
    std::set<std::string> hashes;
    for (int i = 0; i < 1000; ++i) {
        auto x = crypto::generateToken();
        REQUIRE(raws.insert(x.raw).second);
        REQUIRE(hashes.insert(x.hashHex).second);
    }
}

TEST_CASE("constant-time equality matches logical equality", "[crypto]") {
    REQUIRE(crypto::constantTimeEquals("abc123", "abc123"));
    REQUIRE_FALSE(crypto::constantTimeEquals("abc123", "abc124"));
    REQUIRE_FALSE(crypto::constantTimeEquals("abc", "abcd")); // length differs
    REQUIRE(crypto::constantTimeEquals("", ""));

    auto h = crypto::hashToken("some-token-value");
    REQUIRE(crypto::constantTimeEquals(h, crypto::hashToken("some-token-value")));
    REQUIRE_FALSE(crypto::constantTimeEquals(h, crypto::hashToken("other")));
}

TEST_CASE("randomBytes fills the buffer and varies", "[crypto]") {
    std::uint8_t a[32] = {0};
    std::uint8_t b[32] = {0};
    crypto::randomBytes(a, sizeof(a));
    crypto::randomBytes(b, sizeof(b));
    bool differs = false;
    for (std::size_t i = 0; i < sizeof(a); ++i) {
        if (a[i] != b[i]) {
            differs = true;
        }
    }
    REQUIRE(differs);
    // len==0 is a no-op, not an error.
    REQUIRE_NOTHROW(crypto::randomBytes(a, 0));
}

#include "core/btree/key_codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace bisondb;
using namespace bisondb::btree;

namespace {

int memcmpOrder(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::size_t n = std::min(a.size(), b.size());
    int c = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
}

Value randomIndexableValue(std::mt19937& rng) {
    switch (rng() % 8) {
    case 0: return Value();
    case 1: return Value(static_cast<int32_t>(rng()));
    case 2: return Value(static_cast<int64_t>((static_cast<uint64_t>(rng()) << 32) ^ rng()));
    case 3: {
        // Mix of small and full-range doubles, including +-0 and infinities.
        switch (rng() % 5) {
        case 0: return Value(0.0);
        case 1: return Value(-0.0);
        case 2: return Value(std::numeric_limits<double>::infinity() * (rng() % 2 ? 1 : -1));
        case 3: return Value(static_cast<double>(static_cast<int32_t>(rng())) / 7.0);
        default:
            return Value(std::ldexp(static_cast<double>(rng()), static_cast<int>(rng() % 64) - 32));
        }
    }
    case 4: {
        std::string s;
        std::size_t len = rng() % 12;
        for (std::size_t i = 0; i < len; ++i) {
            // Includes 0x00 so the NUL escaping path is exercised.
            s.push_back(static_cast<char>(rng() % 4 == 0 ? 0 : (rng() % 96 + 32)));
        }
        return Value(std::move(s));
    }
    case 5: {
        ObjectId oid;
        for (auto& b : oid.bytes) {
            b = static_cast<uint8_t>(rng());
        }
        return Value(oid);
    }
    case 6: return Value(rng() % 2 == 0);
    default:
        return Value(DateTime{static_cast<int64_t>((static_cast<uint64_t>(rng()) << 32) ^ rng())});
    }
}

} // namespace

TEST_CASE("cross-type ordering follows the tag bytes", "[key_codec]") {
    std::vector<Value> ordered{Value(),             // Null
                               Value(int32_t{5}),   // Number
                               Value("abc"),        // String
                               Value(ObjectId{}),   // ObjectId
                               Value(false),        // Bool
                               Value(DateTime{0})}; // DateTime
    for (std::size_t i = 0; i + 1 < ordered.size(); ++i) {
        REQUIRE(memcmpOrder(encodeKey(ordered[i]), encodeKey(ordered[i + 1])) < 0);
        REQUIRE(compareIndexOrder(ordered[i], ordered[i + 1]) == -1);
    }
}

TEST_CASE("numeric keys order across int32/int64/double", "[key_codec]") {
    REQUIRE(memcmpOrder(encodeKey(Value(int32_t{1})), encodeKey(Value(2.5))) < 0);
    REQUIRE(memcmpOrder(encodeKey(Value(-1.5)), encodeKey(Value(int64_t{-1}))) < 0);
    REQUIRE(memcmpOrder(encodeKey(Value(int32_t{3})), encodeKey(Value(3.0))) == 0);
    REQUIRE(memcmpOrder(encodeKey(Value(-0.0)), encodeKey(Value(0.0))) == 0); // -0 normalized
    double inf = std::numeric_limits<double>::infinity();
    REQUIRE(memcmpOrder(encodeKey(Value(-inf)),
                        encodeKey(Value(std::numeric_limits<double>::lowest()))) < 0);
    REQUIRE(memcmpOrder(encodeKey(Value(std::numeric_limits<double>::max())),
                        encodeKey(Value(inf))) < 0);
}

TEST_CASE("string keys: prefix order and embedded NUL safety", "[key_codec]") {
    REQUIRE(memcmpOrder(encodeKey(Value("a")), encodeKey(Value("aa"))) < 0);
    REQUIRE(memcmpOrder(encodeKey(Value("")), encodeKey(Value("a"))) < 0);
    REQUIRE(memcmpOrder(encodeKey(Value(std::string("a\0a", 3))), encodeKey(Value("ab"))) < 0);
    REQUIRE(memcmpOrder(encodeKey(Value("a")), encodeKey(Value(std::string("a\0", 2)))) < 0);
}

TEST_CASE("NaN and non-indexable types are rejected", "[key_codec]") {
    REQUIRE_THROWS_AS(encodeKey(Value(std::numeric_limits<double>::quiet_NaN())), KeyNotIndexable);
    REQUIRE_THROWS_AS(encodeKey(Value(Document{})), KeyNotIndexable);
    REQUIRE_THROWS_AS(encodeKey(Value(Array{})), KeyNotIndexable);
    REQUIRE_THROWS_AS(encodeKey(Value(Decimal128{})), KeyNotIndexable);
    REQUIRE_FALSE(isIndexableType(Value(std::numeric_limits<double>::quiet_NaN())));
    REQUIRE(isIndexableType(Value(int32_t{1})));
}

TEST_CASE("keys longer than 512 bytes throw KeyTooLong", "[key_codec]") {
    REQUIRE_THROWS_AS(encodeKey(Value(std::string(600, 'x'))), KeyTooLong);
    REQUIRE_NOTHROW(encodeKey(Value(std::string(500, 'x'))));
}

TEST_CASE("composite keys order by (field value, oid) and recover the oid", "[key_codec]") {
    ObjectId lowOid = ObjectId::fromHex("000000000000000000000001");
    ObjectId highOid = ObjectId::fromHex("ffffffffffffffffffffffff");
    auto fieldA = encodeKey(Value(int32_t{1}));
    auto fieldB = encodeKey(Value(int32_t{2}));
    auto k1 = composeIndexKey(fieldA, highOid);
    auto k2 = composeIndexKey(fieldB, lowOid);
    REQUIRE(memcmpOrder(k1, k2) < 0); // field dominates
    auto k3 = composeIndexKey(fieldA, lowOid);
    REQUIRE(memcmpOrder(k3, k1) < 0); // same field: oid breaks the tie
    REQUIRE(oidFromCompositeKey(k1) == highOid);
    REQUIRE(oidFromCompositeKey(k3) == lowOid);
}

TEST_CASE("pairwise ordering fuzz: encoded order matches the reference comparator",
          "[key_codec][fuzz]") {
    for (uint32_t seed : {1u, 42u, 20260612u}) {
        std::mt19937 rng(seed);
        for (int iter = 0; iter < 10000; ++iter) {
            Value a = randomIndexableValue(rng);
            Value b = randomIndexableValue(rng);
            int expected = *compareIndexOrder(a, b);
            int actual = memcmpOrder(encodeKey(a), encodeKey(b));
            int actualSign = actual < 0 ? -1 : (actual > 0 ? 1 : 0);
            if (actualSign != expected) {
                INFO("seed=" << seed << " iter=" << iter);
                REQUIRE(actualSign == expected);
            }
        }
    }
}

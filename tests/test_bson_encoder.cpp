#include "core/bson_decoder.hpp"
#include "core/bson_encoder.hpp"
#include "test_util.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>

using namespace bisondb;
using bisondb::test::bytesToHex;

namespace {

// A document exercising every supported type with several edge values.
Value kitchenSink() {
    return Value(Document{
        {"double", Value(3.14159)},
        {"negzero", Value(-0.0)},
        {"string", Value("hello \xC3\xA9 world")},
        {"empty", Value("")},
        {"embedded", Value(std::string("a\0b", 3))},
        {"doc", Value(Document{{"nested", Value(Document{{"deep", Value(int32_t{1})}})}})},
        {"emptyDoc", Value(Document{})},
        {"arr", Value(Array{Value(int32_t{1}), Value("two"), Value(Array{Value(true)}),
                            Value(Document{{"k", Value()}})})},
        {"emptyArr", Value(Array{})},
        {"oid", Value(ObjectId::fromHex("507f1f77bcf86cd799439011"))},
        {"boolT", Value(true)},
        {"boolF", Value(false)},
        {"date", Value(DateTime{1356351330501})},
        {"epoch", Value(DateTime{0})},
        {"negdate", Value(DateTime{-284643869501})},
        {"null", Value()},
        {"i32min", Value(std::numeric_limits<int32_t>::min())},
        {"i32max", Value(std::numeric_limits<int32_t>::max())},
        {"i64min", Value(std::numeric_limits<int64_t>::min())},
        {"i64max", Value(std::numeric_limits<int64_t>::max())},
        {"dec", Value(Decimal128{{0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x30}})},
    });
}

} // namespace

TEST_CASE("encode throws on non-document values", "[encoder]") {
    REQUIRE_THROWS_AS(encodeDocument(Value(int32_t{1})), TypeError);
    REQUIRE_THROWS_AS(encodeDocument(Value(Array{Value(int32_t{1})})), TypeError);
    REQUIRE_THROWS_AS(encodeDocument(Value()), TypeError);
}

TEST_CASE("encode empty document is the 5-byte minimal document", "[encoder]") {
    REQUIRE(bytesToHex(encodeDocument(Value(Document{}))) == "0500000000");
}

TEST_CASE("decode(encode(v)) == v across all types", "[encoder]") {
    Value original = kitchenSink();
    std::vector<uint8_t> bytes = encodeDocument(original);
    Value decoded = decodeDocument(bytes);
    REQUIRE(decoded == original);
}

TEST_CASE("encode then decode preserves NaN doubles", "[encoder]") {
    Value v(Document{{"d", Value(std::numeric_limits<double>::quiet_NaN())}});
    Value decoded = decodeDocument(encodeDocument(v));
    REQUIRE(std::isnan(decoded.asDocument().find("d")->get<double>()));
}

TEST_CASE("encode generates sequential array index keys", "[encoder]") {
    Value v(Document{{"a", Value(Array{Value(int32_t{5}), Value(int32_t{6})})}});
    // {"a": [5, 6]} with inner keys "0" and "1".
    REQUIRE(bytesToHex(encodeDocument(v)) ==
            "1B0000000461001300000010300005000000103100060000000000");
}

TEST_CASE("encode rejects keys containing NUL bytes", "[encoder]") {
    Value v(Document{{std::string("a\0b", 3), Value(int32_t{1})}});
    REQUIRE_THROWS_AS(encodeDocument(v), EncodeError);
}

TEST_CASE("encode rejects nesting deeper than 200", "[encoder]") {
    Value v(Document{});
    for (int i = 0; i < 201; ++i) {
        v = Value(Document{{"a", std::move(v)}});
    }
    REQUIRE_THROWS_AS(encodeDocument(v), EncodeError);
}

TEST_CASE("size prefixes are backpatched correctly for nested content", "[encoder]") {
    std::vector<uint8_t> bytes = encodeDocument(kitchenSink());
    // The declared top-level size must equal the actual byte count, and the
    // document must round-trip through the (independently validated) decoder
    // when handed exactly those bytes.
    uint32_t declared = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                        (static_cast<uint32_t>(bytes[2]) << 16) |
                        (static_cast<uint32_t>(bytes[3]) << 24);
    REQUIRE(declared == bytes.size());
    REQUIRE_NOTHROW(decodeDocument(bytes));
}

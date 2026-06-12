#include "core/value.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace bisondb;

TEST_CASE("Value default-constructs to Null", "[value]") {
    Value v;
    REQUIRE(v.type() == Type::Null);
    REQUIRE(v.isNull());
    REQUIRE(v.is<std::monostate>());
}

TEST_CASE("Value reports the right type for every alternative", "[value]") {
    REQUIRE(Value(1.5).type() == Type::Double);
    REQUIRE(Value("hi").type() == Type::String);
    REQUIRE(Value(Document{}).type() == Type::Document);
    REQUIRE(Value(Array{}).type() == Type::Array);
    REQUIRE(Value(ObjectId{}).type() == Type::ObjectId);
    REQUIRE(Value(true).type() == Type::Bool);
    REQUIRE(Value(DateTime{0}).type() == Type::DateTime);
    REQUIRE(Value(std::monostate{}).type() == Type::Null);
    REQUIRE(Value(int32_t{1}).type() == Type::Int32);
    REQUIRE(Value(int64_t{1}).type() == Type::Int64);
    REQUIRE(Value(Decimal128{}).type() == Type::Decimal128);
}

TEST_CASE("get returns the stored value and throws TypeError on mismatch", "[value]") {
    Value v(int32_t{42});
    REQUIRE(v.is<int32_t>());
    REQUIRE(v.get<int32_t>() == 42);
    REQUIRE_FALSE(v.is<int64_t>());
    REQUIRE_THROWS_AS(v.get<int64_t>(), TypeError);
    REQUIRE_THROWS_AS(v.get<std::string>(), TypeError);
    REQUIRE_THROWS_AS(v.asDocument(), TypeError);
    REQUIRE_THROWS_AS(v.asArray(), TypeError);
}

TEST_CASE("Document preserves insertion order and finds by key", "[value]") {
    Document doc{{"zebra", Value(int32_t{1})}, {"apple", Value("two")}, {"mango", Value(true)}};
    REQUIRE(doc.size() == 3);
    REQUIRE(doc[0].first == "zebra");
    REQUIRE(doc[1].first == "apple");
    REQUIRE(doc[2].first == "mango");

    REQUIRE(doc.contains("apple"));
    REQUIRE_FALSE(doc.contains("missing"));
    REQUIRE(doc.find("missing") == nullptr);
    const Value* apple = doc.find("apple");
    REQUIRE(apple != nullptr);
    REQUIRE(apple->get<std::string>() == "two");
}

TEST_CASE("Document find returns the first match for duplicated keys", "[value]") {
    Document doc;
    doc.append("k", Value(int32_t{1}));
    doc.append("k", Value(int32_t{2}));
    REQUIRE(doc.find("k")->get<int32_t>() == 1);
}

TEST_CASE("ObjectId hex round-trip", "[value]") {
    ObjectId oid = ObjectId::fromHex("507f1f77bcf86cd799439011");
    REQUIRE(oid.toHex() == "507f1f77bcf86cd799439011");
    REQUIRE(oid.bytes[0] == 0x50);
    REQUIRE(oid.bytes[11] == 0x11);

    // Uppercase input is accepted; output is lowercase.
    REQUIRE(ObjectId::fromHex("507F1F77BCF86CD799439011") == oid);
}

TEST_CASE("ObjectId fromHex rejects bad input", "[value]") {
    REQUIRE_THROWS_AS(ObjectId::fromHex(""), TypeError);
    REQUIRE_THROWS_AS(ObjectId::fromHex("507f1f77bcf86cd79943901"), TypeError);   // 23 chars
    REQUIRE_THROWS_AS(ObjectId::fromHex("507f1f77bcf86cd7994390111"), TypeError); // 25 chars
    REQUIRE_THROWS_AS(ObjectId::fromHex("507f1f77bcf86cd79943901g"), TypeError);  // non-hex
}

TEST_CASE("Value equality is deep and type-sensitive", "[value]") {
    REQUIRE(Value(int32_t{1}) == Value(int32_t{1}));
    REQUIRE(Value(int32_t{1}) != Value(int64_t{1})); // same number, different BSON type
    REQUIRE(Value("a") != Value("b"));
    REQUIRE(Value() == Value(std::monostate{}));

    Value nested(Document{
        {"arr", Value(Array{Value(int32_t{1}), Value("x")})},
        {"sub", Value(Document{{"k", Value(true)}})},
    });
    Value same(Document{
        {"arr", Value(Array{Value(int32_t{1}), Value("x")})},
        {"sub", Value(Document{{"k", Value(true)}})},
    });
    REQUIRE(nested == same);

    // Key order matters for document equality.
    Value reordered(Document{
        {"sub", Value(Document{{"k", Value(true)}})},
        {"arr", Value(Array{Value(int32_t{1}), Value("x")})},
    });
    REQUIRE(nested != reordered);
}

TEST_CASE("Wrapper structs compare by contents", "[value]") {
    REQUIRE(DateTime{100} == DateTime{100});
    REQUIRE(DateTime{100} != DateTime{101});
    Decimal128 a;
    Decimal128 b;
    b.bytes[15] = 0x30;
    REQUIRE(a != b);
}

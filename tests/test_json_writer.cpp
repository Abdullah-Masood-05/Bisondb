#include "core/decimal128.hpp"
#include "core/json_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string_view>

using namespace bisondb;

namespace {

Value dec(std::string_view s) {
    return Value(decimal128FromString(s));
}

} // namespace

TEST_CASE("relaxed mode writes plain numbers", "[json_writer]") {
    Value v(
        Document{{"i", Value(int32_t{42})}, {"l", Value(int64_t{9000000000})}, {"d", Value(2.5)}});
    REQUIRE(toJson(v) == R"({"i":42,"l":9000000000,"d":2.5})");
}

TEST_CASE("canonical mode wraps all numbers", "[json_writer]") {
    Value v(
        Document{{"i", Value(int32_t{42})}, {"l", Value(int64_t{9000000000})}, {"d", Value(2.5)}});
    REQUIRE(
        toJson(v, JsonMode::Canonical) ==
        R"({"i":{"$numberInt":"42"},"l":{"$numberLong":"9000000000"},"d":{"$numberDouble":"2.5"}})");
}

TEST_CASE("integral doubles keep a .0 suffix in both modes", "[json_writer]") {
    REQUIRE(toJson(Value(1.0)) == "1.0");
    REQUIRE(toJson(Value(-0.0)) == "-0.0");
    REQUIRE(toJson(Value(1.0), JsonMode::Canonical) == R"({"$numberDouble":"1.0"})");
}

TEST_CASE("non-finite doubles always use the $numberDouble wrapper", "[json_writer]") {
    double nan = std::numeric_limits<double>::quiet_NaN();
    double inf = std::numeric_limits<double>::infinity();
    REQUIRE(toJson(Value(nan)) == R"({"$numberDouble":"NaN"})");
    REQUIRE(toJson(Value(inf)) == R"({"$numberDouble":"Infinity"})");
    REQUIRE(toJson(Value(-inf)) == R"({"$numberDouble":"-Infinity"})");
    REQUIRE(toJson(Value(nan), JsonMode::Canonical) == R"({"$numberDouble":"NaN"})");
}

TEST_CASE("bool and null are bare literals in both modes", "[json_writer]") {
    Value v(Document{{"t", Value(true)}, {"n", Value()}});
    REQUIRE(toJson(v) == R"({"t":true,"n":null})");
    REQUIRE(toJson(v, JsonMode::Canonical) == R"({"t":true,"n":null})");
}

TEST_CASE("ObjectId renders as $oid", "[json_writer]") {
    Value v(ObjectId::fromHex("507f1f77bcf86cd799439011"));
    REQUIRE(toJson(v) == R"({"$oid":"507f1f77bcf86cd799439011"})");
}

TEST_CASE("DateTime in relaxed mode uses ISO-8601 for years 1970-9999", "[json_writer]") {
    REQUIRE(toJson(Value(DateTime{0})) == R"({"$date":"1970-01-01T00:00:00Z"})");
    REQUIRE(toJson(Value(DateTime{1356351330501})) == R"({"$date":"2012-12-24T12:15:30.501Z"})");
    // Milliseconds are omitted when zero.
    REQUIRE(toJson(Value(DateTime{1356351330000})) == R"({"$date":"2012-12-24T12:15:30Z"})");
}

TEST_CASE("DateTime outside 1970-9999 falls back to $numberLong in relaxed mode", "[json_writer]") {
    REQUIRE(toJson(Value(DateTime{-1})) == R"({"$date":{"$numberLong":"-1"}})");
    // Year 10000 and beyond.
    REQUIRE(toJson(Value(DateTime{253402300800000})) ==
            R"({"$date":{"$numberLong":"253402300800000"}})");
    // 9999-12-31T23:59:59.999Z is still in range.
    REQUIRE(toJson(Value(DateTime{253402300799999})) == R"({"$date":"9999-12-31T23:59:59.999Z"})");
}

TEST_CASE("DateTime in canonical mode always uses $numberLong", "[json_writer]") {
    REQUIRE(toJson(Value(DateTime{1356351330501}), JsonMode::Canonical) ==
            R"({"$date":{"$numberLong":"1356351330501"}})");
}

TEST_CASE("Decimal128 renders as $numberDecimal", "[json_writer]") {
    REQUIRE(toJson(dec("1")) == R"({"$numberDecimal":"1"})");
    REQUIRE(toJson(dec("-10.5")) == R"({"$numberDecimal":"-10.5"})");
}

TEST_CASE("decimal128 string rendering follows the Extended JSON rules", "[json_writer]") {
    auto render = [](std::string_view in) { return decimal128ToString(decimal128FromString(in)); };
    REQUIRE(render("0") == "0");
    REQUIRE(render("-0") == "-0");
    REQUIRE(render("1") == "1");
    REQUIRE(render("1.0") == "1.0");
    REQUIRE(render("0.001") == "0.001");
    REQUIRE(render("12345E-2") == "123.45");
    // Positive exponents and adjusted exponents below -6 switch to scientific.
    REQUIRE(render("1E+3") == "1E+3");
    REQUIRE(render("5E-7") == "5E-7");
    REQUIRE(render("0E+300") == "0E+300");
    // Range extremes.
    REQUIRE(render("1E-6176") == "1E-6176");
    REQUIRE(render("9.999999999999999999999999999999999E+6144") ==
            "9.999999999999999999999999999999999E+6144");
    // Specials. The sign of NaN is not rendered.
    REQUIRE(render("NaN") == "NaN");
    REQUIRE(render("-NaN") == "NaN");
    REQUIRE(render("Infinity") == "Infinity");
    REQUIRE(render("-inf") == "-Infinity");
}

TEST_CASE("non-canonical decimal128 bit patterns render a zero significand", "[json_writer]") {
    // Combination field starting with 11 (the densely-packed-decimal style
    // pattern): the value reads as zero with the encoded exponent.
    Decimal128 d{};
    d.bytes[15] = 0x60; // high word = 0x6000...0000 -> biased exponent 0
    REQUIRE(decimal128ToString(d) == "0E-6176");
}

TEST_CASE("decimal128FromString rejects invalid input", "[json_writer]") {
    REQUIRE_THROWS_AS(decimal128FromString(""), TypeError);
    REQUIRE_THROWS_AS(decimal128FromString("abc"), TypeError);
    REQUIRE_THROWS_AS(decimal128FromString("1.2.3"), TypeError);
    REQUIRE_THROWS_AS(decimal128FromString("1E"), TypeError);
    // 35 non-zero significant digits cannot be represented exactly.
    REQUIRE_THROWS_AS(decimal128FromString("12345678901234567890123456789012345"), TypeError);
    // ... but excess trailing zeros fold into the exponent.
    REQUIRE(decimal128ToString(decimal128FromString("12345678901234567890123456789012340")) ==
            "1.234567890123456789012345678901234E+34");
    REQUIRE_THROWS_AS(decimal128FromString("1E+99999"), TypeError);
    REQUIRE_THROWS_AS(decimal128FromString("1E-99999"), TypeError);
    // Zero clamps instead of overflowing.
    REQUIRE(decimal128ToString(decimal128FromString("0E+99999")) == "0E+6111");
}

TEST_CASE("string escaping", "[json_writer]") {
    REQUIRE(toJson(Value("say \"hi\"")) == R"("say \"hi\"")");
    REQUIRE(toJson(Value("back\\slash")) == R"("back\\slash")");
    // Control characters are escaped as \uXXXX.
    REQUIRE(toJson(Value(std::string("a\0b\tc\n", 6))) == "\"a\\u0000b\\u0009c\\u000a\"");
    // Valid UTF-8 passes through unescaped.
    REQUIRE(toJson(Value("caf\xC3\xA9 \xE2\x98\x86")) == "\"caf\xC3\xA9 \xE2\x98\x86\"");
}

TEST_CASE("keys are escaped like string values", "[json_writer]") {
    Value v(Document{{"we\"ird", Value(int32_t{1})}});
    REQUIRE(toJson(v) == R"({"we\"ird":1})");
}

TEST_CASE("empty containers render compactly even when pretty", "[json_writer]") {
    Value v(Document{{"d", Value(Document{})}, {"a", Value(Array{})}});
    REQUIRE(toJson(v) == R"({"d":{},"a":[]})");
    REQUIRE(toJson(v, JsonMode::Relaxed, true) == "{\n  \"d\": {},\n  \"a\": []\n}");
}

TEST_CASE("pretty mode indents by two spaces", "[json_writer]") {
    Value v(Document{{"a", Value(int32_t{1})},
                     {"b", Value(Array{Value(int32_t{2}), Value(Document{{"c", Value("x")}})})}});
    const char* expected = "{\n"
                           "  \"a\": 1,\n"
                           "  \"b\": [\n"
                           "    2,\n"
                           "    {\n"
                           "      \"c\": \"x\"\n"
                           "    }\n"
                           "  ]\n"
                           "}";
    REQUIRE(toJson(v, JsonMode::Relaxed, true) == expected);
}

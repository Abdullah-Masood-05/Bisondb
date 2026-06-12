#include "core/decimal128.hpp"
#include "core/json_parser.hpp"
#include "core/json_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <limits>
#include <string>

using namespace bisondb;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("parse scalars", "[json_parser]") {
    REQUIRE(parseJson("true") == Value(true));
    REQUIRE(parseJson("false") == Value(false));
    REQUIRE(parseJson("null") == Value());
    REQUIRE(parseJson("\"hi\"") == Value("hi"));
    REQUIRE(parseJson(" 42 ") == Value(int32_t{42}));
}

TEST_CASE("parse objects and arrays preserving order", "[json_parser]") {
    Value v = parseJson(R"({"z": 1, "a": [true, null, "s"], "m": {}})");
    const Document& doc = v.asDocument();
    REQUIRE(doc.size() == 3);
    REQUIRE(doc[0].first == "z");
    REQUIRE(doc[1].first == "a");
    REQUIRE(doc[2].first == "m");
    const Array& arr = doc.find("a")->asArray();
    REQUIRE(arr.size() == 3);
    REQUIRE(arr[0] == Value(true));
    REQUIRE(arr[1] == Value());
    REQUIRE(arr[2] == Value("s"));
    REQUIRE(doc.find("m")->asDocument().empty());
}

TEST_CASE("number type selection", "[json_parser]") {
    SECTION("int32 range -> Int32") {
        REQUIRE(parseJson("0") == Value(int32_t{0}));
        REQUIRE(parseJson("-1") == Value(int32_t{-1}));
        REQUIRE(parseJson("2147483647") == Value(int32_t{2147483647}));
        REQUIRE(parseJson("-2147483648") == Value(std::numeric_limits<int32_t>::min()));
    }
    SECTION("int64 range -> Int64") {
        REQUIRE(parseJson("2147483648") == Value(int64_t{2147483648}));
        REQUIRE(parseJson("-2147483649") == Value(int64_t{-2147483649}));
        REQUIRE(parseJson("9223372036854775807") == Value(std::numeric_limits<int64_t>::max()));
    }
    SECTION("beyond int64 -> Double") {
        Value v = parseJson("9223372036854775808");
        REQUIRE(v.is<double>());
        REQUIRE(v.get<double>() == 9223372036854775808.0);
    }
    SECTION("decimal point or exponent -> Double") {
        REQUIRE(parseJson("1.5") == Value(1.5));
        REQUIRE(parseJson("1e3") == Value(1000.0));
        REQUIRE(parseJson("2E-1") == Value(0.2));
        REQUIRE(parseJson("1.0") == Value(1.0));
        REQUIRE(parseJson("-0.0").is<double>());
    }
}

TEST_CASE("string escape sequences", "[json_parser]") {
    REQUIRE(parseJson(R"("a\"b\\c\/d\be\ff\ng\rh\ti")") == Value("a\"b\\c/d\be\ff\ng\rh\ti"));
    // \u escapes for BMP characters: U+00E9 and U+2606.
    REQUIRE(parseJson("\"A\\u00e9\\u2606\"") == Value("A\xC3\xA9\xE2\x98\x86"));
    // NUL escapes are representable in std::string.
    REQUIRE(parseJson("\"a\\u0000b\"") == Value(std::string("a\0b", 3)));
}

TEST_CASE("surrogate pairs combine into supplementary-plane characters", "[json_parser]") {
    // U+1F600 GRINNING FACE as an escaped surrogate pair, and as raw UTF-8.
    REQUIRE(parseJson("\"\\uD83D\\uDE00\"") == Value("\xF0\x9F\x98\x80"));
    REQUIRE(parseJson("\"\xF0\x9F\x98\x80\"") == Value("\xF0\x9F\x98\x80"));
}

TEST_CASE("invalid surrogates are rejected", "[json_parser]") {
    REQUIRE_THROWS_AS(parseJson(R"("\uD83D")"), JsonParseError);  // lone high
    REQUIRE_THROWS_AS(parseJson(R"("\uDE00")"), JsonParseError);  // lone low
    REQUIRE_THROWS_AS(parseJson(R"("\uD83DA")"), JsonParseError); // high + non-low
}

TEST_CASE("Extended JSON wrappers fold into typed values", "[json_parser]") {
    SECTION("$oid") {
        Value v = parseJson(R"({"$oid":"507f1f77bcf86cd799439011"})");
        REQUIRE(v.get<ObjectId>().toHex() == "507f1f77bcf86cd799439011");
    }
    SECTION("$numberInt / $numberLong / $numberDouble / $numberDecimal") {
        REQUIRE(parseJson(R"({"$numberInt":"-5"})") == Value(int32_t{-5}));
        REQUIRE(parseJson(R"({"$numberLong":"9000000000"})") == Value(int64_t{9000000000}));
        REQUIRE(parseJson(R"({"$numberDouble":"2.5"})") == Value(2.5));
        REQUIRE(std::isnan(parseJson(R"({"$numberDouble":"NaN"})").get<double>()));
        REQUIRE(parseJson(R"({"$numberDouble":"-Infinity"})").get<double>() ==
                -std::numeric_limits<double>::infinity());
        REQUIRE(parseJson(R"({"$numberDecimal":"10.5"})") == Value(decimal128FromString("10.5")));
    }
    SECTION("$date with ISO string") {
        REQUIRE(parseJson(R"({"$date":"2012-12-24T12:15:30.501Z"})") ==
                Value(DateTime{1356351330501}));
        REQUIRE(parseJson(R"({"$date":"1970-01-01T00:00:00Z"})") == Value(DateTime{0}));
        // Timezone offsets are honoured.
        REQUIRE(parseJson(R"({"$date":"2012-12-24T13:15:30.501+01:00"})") ==
                Value(DateTime{1356351330501}));
    }
    SECTION("$date with $numberLong") {
        REQUIRE(parseJson(R"({"$date":{"$numberLong":"-62135593139000"}})") ==
                Value(DateTime{-62135593139000}));
    }
    SECTION("wrappers nested inside documents and arrays") {
        Value v = parseJson(R"({"ids":[{"$oid":"507f1f77bcf86cd799439011"}]})");
        REQUIRE(v.asDocument().find("ids")->asArray()[0].is<ObjectId>());
    }
}

TEST_CASE("unknown $-keys stay as plain documents", "[json_parser]") {
    Value v = parseJson(R"({"$regex":"abc"})");
    REQUIRE(v.is<Document>());
    REQUIRE(v.asDocument().find("$regex")->get<std::string>() == "abc");

    // Multi-key documents are never folded, even with known $-keys.
    Value multi = parseJson(R"({"$oid":"507f1f77bcf86cd799439011","x":1})");
    REQUIRE(multi.is<Document>());
    REQUIRE(multi.asDocument().size() == 2);
}

TEST_CASE("malformed extended JSON wrappers are errors", "[json_parser]") {
    REQUIRE_THROWS_AS(parseJson(R"({"$oid":"zzz"})"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson(R"({"$oid":12})"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson(R"({"$numberInt":"abc"})"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson(R"({"$numberInt":"2147483648"})"), JsonParseError); // overflow
    REQUIRE_THROWS_AS(parseJson(R"({"$numberLong":12})"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson(R"({"$numberDecimal":"x"})"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson(R"({"$date":"not-a-date"})"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson(R"({"$date":true})"), JsonParseError);
}

TEST_CASE("malformed JSON is rejected with line and column info", "[json_parser]") {
    REQUIRE_THROWS_AS(parseJson(""), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("{"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("[1,]"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("{\"a\" 1}"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("tru"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("\"unterminated"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("01"), JsonParseError); // leading zero -> trailing garbage
    REQUIRE_THROWS_AS(parseJson("1."), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("-"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("1e"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson(R"("bad \q escape")"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("\"raw\ncontrol\""), JsonParseError);

    try {
        parseJson("{\n  \"a\": @\n}");
        FAIL("expected JsonParseError");
    } catch (const JsonParseError& e) {
        REQUIRE(e.line() == 2);
        REQUIRE(e.column() == 8);
        REQUIRE_THAT(e.what(), ContainsSubstring("line 2"));
    }
}

TEST_CASE("trailing garbage after the top-level value is rejected", "[json_parser]") {
    REQUIRE_THROWS_MATCHES(parseJson("{} x"), JsonParseError,
                           Catch::Matchers::MessageMatches(ContainsSubstring("trailing")));
    REQUIRE_THROWS_AS(parseJson("1 2"), JsonParseError);
    REQUIRE_NOTHROW(parseJson("{}  \n\t "));
}

TEST_CASE("nesting depth above 200 is rejected", "[json_parser]") {
    std::string deep(201, '[');
    deep += std::string(201, ']');
    REQUIRE_THROWS_MATCHES(parseJson(deep), JsonParseError,
                           Catch::Matchers::MessageMatches(ContainsSubstring("depth")));

    std::string ok(200, '[');
    ok += std::string(200, ']');
    REQUIRE_NOTHROW(parseJson(ok));
}

TEST_CASE("parseJsonOne reports consumed bytes for streaming", "[json_parser]") {
    std::string text = "{\"a\":1}\n{\"b\":2}";
    std::size_t consumed = 0;
    Value first = parseJsonOne(text, consumed);
    REQUIRE(first.asDocument().find("a")->get<int32_t>() == 1);
    REQUIRE(consumed == 7);

    std::string_view rest = std::string_view(text).substr(consumed);
    Value second = parseJsonOne(rest, consumed);
    REQUIRE(second.asDocument().find("b")->get<int32_t>() == 2);
}

TEST_CASE("writer output parses back to the identical value", "[json_parser]") {
    Value original(Document{
        {"oid", Value(ObjectId::fromHex("507f1f77bcf86cd799439011"))},
        {"date", Value(DateTime{1356351330501})},
        {"i", Value(int32_t{-7})},
        {"l", Value(int64_t{1} << 40)},
        {"d", Value(0.5)},
        {"dec", Value(decimal128FromString("-1.5E+3"))},
        {"s", Value("text")},
        {"arr", Value(Array{Value(true), Value()})},
    });
    for (bool pretty : {false, true}) {
        Value reparsed = parseJson(toJson(original, JsonMode::Canonical, pretty));
        REQUIRE(reparsed == original);
    }
}

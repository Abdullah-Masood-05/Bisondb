#include "core/json_parser.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace bisondb;

namespace {

JsonParseOptions relaxed() {
    JsonParseOptions o;
    o.relaxed = true;
    return o;
}

} // namespace

TEST_CASE("relaxed: unquoted object keys", "[json_relaxed]") {
    Value v = parseJson("{name: 1, _under: 2, $gt: 3, mix3d$: 4}", relaxed());
    const Document& d = v.asDocument();
    REQUIRE(d.find("name")->get<int32_t>() == 1);
    REQUIRE(d.find("_under")->get<int32_t>() == 2);
    REQUIRE(d.find("$gt")->get<int32_t>() == 3);
    REQUIRE(d.find("mix3d$")->get<int32_t>() == 4);
    // Operators nest unquoted.
    Value f = parseJson("{cgpa: {$gt: 3.5}}", relaxed());
    REQUIRE(f.asDocument().find("cgpa")->asDocument().find("$gt")->get<double>() == 3.5);
}

TEST_CASE("relaxed: single-quoted strings", "[json_relaxed]") {
    REQUIRE(parseJson("'hi'", relaxed()) == Value("hi"));
    REQUIRE(parseJson(R"({'a': 'b "quoted"'})", relaxed()) ==
            parseJson(R"({"a": "b \"quoted\""})"));
    // Same escape set works, plus \' for the quote itself.
    REQUIRE(parseJson(R"('tab\there')", relaxed()) == Value("tab\there"));
    REQUIRE(parseJson(R"('don\'t')", relaxed()) == Value("don't"));
}

TEST_CASE("relaxed: trailing commas", "[json_relaxed]") {
    REQUIRE(parseJson("[1, 2, 3,]", relaxed()) == parseJson("[1, 2, 3]"));
    REQUIRE(parseJson("{a: 1, b: 2,}", relaxed()) == parseJson(R"({"a": 1, "b": 2})"));
    REQUIRE(parseJson("{a: [1,], }", relaxed()) == parseJson(R"({"a": [1]})"));
}

TEST_CASE("strict mode rejects every relaxation", "[json_relaxed]") {
    REQUIRE_THROWS_AS(parseJson("{name: 1}"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("'hi'"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("[1, 2,]"), JsonParseError);
    REQUIRE_THROWS_AS(parseJson("{\"a\": 1,}"), JsonParseError);
}

TEST_CASE("still illegal in both modes", "[json_relaxed]") {
    for (bool r : {false, true}) {
        JsonParseOptions o;
        o.relaxed = r;
        REQUIRE_THROWS_AS(parseJson("{a: b}", o), JsonParseError);  // bare value
        REQUIRE_THROWS_AS(parseJson("{a 1}", o), JsonParseError);   // missing ':'
        REQUIRE_THROWS_AS(parseJson("{3a: 1}", o), JsonParseError); // bad ident start
        REQUIRE_THROWS_AS(parseJson("{,}", o), JsonParseError);     // lone comma
        REQUIRE_THROWS_AS(parseJson("[,1]", o), JsonParseError);    // leading comma
        REQUIRE_THROWS_AS(parseJson("{a: 1", o), JsonParseError);   // unterminated
    }
}

TEST_CASE("relaxed parseJsonOne reports consumed bytes", "[json_relaxed]") {
    std::size_t consumed = 0;
    Value v = parseJsonOne("{a: 1} trailing", consumed, relaxed());
    REQUIRE(v.asDocument().find("a")->get<int32_t>() == 1);
    REQUIRE(consumed == 6);
}

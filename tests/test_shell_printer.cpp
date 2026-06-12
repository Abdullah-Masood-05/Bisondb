#include "core/json_parser.hpp"
#include "core/json_writer.hpp"
#include "shell/printer.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace bisondb;
using namespace bisondb::shell;

TEST_CASE("colorize golden: keys, strings, numbers, literals", "[shell_printer]") {
    std::string pretty =
        toJson(parseJson(R"({"name":"ada","age":36,"ok":true,"x":null})"), JsonMode::Relaxed, true);
    std::string colored = colorizeJson(pretty, true);
    const char* expected = "{\n"
                           "  \x1b[36m\"name\"\x1b[0m: \x1b[32m\"ada\"\x1b[0m,\n"
                           "  \x1b[36m\"age\"\x1b[0m: \x1b[33m36\x1b[0m,\n"
                           "  \x1b[36m\"ok\"\x1b[0m: \x1b[35mtrue\x1b[0m,\n"
                           "  \x1b[36m\"x\"\x1b[0m: \x1b[35mnull\x1b[0m\n"
                           "}";
    REQUIRE(colored == expected);
}

TEST_CASE("colorize golden: $oid and $date payloads are dim", "[shell_printer]") {
    std::string pretty = toJson(parseJson(R"({"_id":{"$oid":"507f1f77bcf86cd799439011"}})"),
                                JsonMode::Relaxed, true);
    std::string colored = colorizeJson(pretty, true);
    // Extended JSON wrappers print inline even in pretty mode (Phase 1
    // printer behavior).
    const char* expected = "{\n"
                           "  \x1b[36m\"_id\"\x1b[0m: {\x1b[36m\"$oid\"\x1b[0m: "
                           "\x1b[2m\"507f1f77bcf86cd799439011\"\x1b[0m}\n"
                           "}";
    REQUIRE(colored == expected);
}

TEST_CASE("no-color output is byte-identical to the pretty printer", "[shell_printer]") {
    std::string pretty =
        toJson(parseJson(R"({"a":[1,-2.5,"s"],"b":false})"), JsonMode::Relaxed, true);
    REQUIRE(colorizeJson(pretty, false) == pretty);
}

TEST_CASE("stripAnsi removes every escape the colorizer emits", "[shell_printer]") {
    std::string pretty = toJson(
        parseJson(
            R"({"s":"hi","n":-1.5e3,"t":true,"f":false,"z":null,"o":{"$oid":"507f1f77bcf86cd799439011"}})"),
        JsonMode::Relaxed, true);
    REQUIRE(stripAnsi(colorizeJson(pretty, true)) == pretty);
    REQUIRE(stripAnsi("plain") == "plain");
    REQUIRE(stripAnsi("\x1b[31mred\x1b[0m") == "red");
}

TEST_CASE("strings containing braces or escaped quotes stay intact", "[shell_printer]") {
    std::string pretty =
        toJson(parseJson(R"({"weird":"a \"quoted\" {brace} 123"})"), JsonMode::Relaxed, true);
    std::string colored = colorizeJson(pretty, true);
    REQUIRE(stripAnsi(colored) == pretty);
    // The embedded 123 must not be colored as a number.
    REQUIRE(colored.find("\x1b[32m\"a \\\"quoted\\\" {brace} 123\"\x1b[0m") != std::string::npos);
}

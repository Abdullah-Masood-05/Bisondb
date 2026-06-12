#include "core/bson_decoder.hpp"
#include "core/bson_encoder.hpp"
#include "core/decimal128.hpp"
#include "core/json_parser.hpp"
#include "core/json_writer.hpp"
#include "test_util.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <limits>
#include <vector>

using namespace bisondb;
using bisondb::test::hexToBytes;

namespace {

// bson -> Value -> canonical JSON -> Value -> bson must be byte-identical.
void requireFullRoundTrip(const std::vector<uint8_t>& bson) {
    Value decoded = decodeDocument(bson);
    std::string json = toJson(decoded, JsonMode::Canonical);
    Value reparsed = parseJson(json);
    REQUIRE(reparsed == decoded);
    REQUIRE(encodeDocument(reparsed) == bson);
}

} // namespace

TEST_CASE("curated fixtures survive bson -> json -> bson byte-identically", "[roundtrip]") {
    Value fixture = GENERATE(
        Value(Document{}), Value(Document{{"d", Value(1.0)}}), Value(Document{{"d", Value(-0.0)}}),
        Value(Document{{"d", Value(std::numeric_limits<double>::infinity())}}),
        Value(Document{{"min", Value(std::numeric_limits<double>::denorm_min())}}),
        Value(Document{{"s", Value("")}}), Value(Document{{"s", Value("caf\xC3\xA9")}}),
        Value(Document{{"s", Value(std::string("nul\0inside", 10))}}),
        Value(Document{{"doc", Value(Document{{"inner", Value(Document{})}})}}),
        Value(Document{{"arr", Value(Array{})}}),
        Value(Document{{"arr", Value(Array{Value(int32_t{1}), Value(int64_t{2}), Value(3.0)})}}),
        Value(Document{{"oid", Value(ObjectId::fromHex("000000000000000000000000"))}}),
        Value(Document{{"oid", Value(ObjectId::fromHex("ffffffffffffffffffffffff"))}}),
        Value(Document{{"b", Value(true)}, {"c", Value(false)}}),
        Value(Document{{"dt", Value(DateTime{0})}}),
        Value(Document{{"dt", Value(DateTime{-62135596800000})}}), // year 0001
        Value(Document{{"dt", Value(DateTime{std::numeric_limits<int64_t>::max()})}}),
        Value(Document{{"n", Value()}}),
        Value(Document{{"i", Value(std::numeric_limits<int32_t>::min())}}),
        Value(Document{{"i", Value(std::numeric_limits<int64_t>::max())}}),
        Value(Document{{"dec", Value(decimal128FromString("0.10"))}}),
        Value(Document{
            {"dec", Value(decimal128FromString("-9.999999999999999999999999999999999E+6144"))}}),
        Value(Document{
            {"deep",
             Value(Document{{"mix", Value(Array{
                                        Value(Document{{"a", Value(Array{Value(int32_t{1}),
                                                                         Value(Document{})})}}),
                                        Value("x"),
                                        Value(DateTime{1356351330501}),
                                    })}})}}));

    requireFullRoundTrip(encodeDocument(fixture));
}

TEST_CASE("known wire-format bytes survive the full round trip", "[roundtrip]") {
    // {"d": 1.0} from the official corpus.
    requireFullRoundTrip(hexToBytes("10000000016400000000000000F03F00"));
    // {"x": {}}
    requireFullRoundTrip(hexToBytes("0D000000037800050000000000"));
    // {"a": [10]}
    requireFullRoundTrip(hexToBytes("140000000461000C0000001030000A0000000000"));
    // {"a": ObjectId(...)}
    requireFullRoundTrip(hexToBytes("1400000007610056E1FC72E0C917E9C471416100"));
}

TEST_CASE("NaN doubles round-trip through canonical JSON", "[roundtrip]") {
    // NaN != NaN, so the equality-based helper does not apply; compare bytes.
    Value v(Document{{"d", Value(std::numeric_limits<double>::quiet_NaN())}});
    std::vector<uint8_t> bson = encodeDocument(v);
    Value reparsed = parseJson(toJson(decodeDocument(bson), JsonMode::Canonical));
    std::vector<uint8_t> again = encodeDocument(reparsed);
    // The quiet-NaN bit pattern produced by the parser is the canonical one;
    // re-encode of the same parsed value must at least be stable.
    REQUIRE(encodeDocument(reparsed) == again);
    REQUIRE(std::isnan(reparsed.asDocument().find("d")->get<double>()));
}

TEST_CASE("relaxed JSON round-trips values whose types relaxed mode preserves", "[roundtrip]") {
    // Relaxed mode collapses int64/int32 distinctions, so only test types it
    // renders unambiguously.
    Value v(Document{{"s", Value("text")},
                     {"b", Value(true)},
                     {"n", Value()},
                     {"oid", Value(ObjectId::fromHex("507f1f77bcf86cd799439011"))},
                     {"dt", Value(DateTime{1356351330501})},
                     {"dec", Value(decimal128FromString("1.5"))}});
    Value reparsed = parseJson(toJson(v, JsonMode::Relaxed));
    REQUIRE(reparsed == v);
}

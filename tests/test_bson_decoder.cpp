#include "core/bson_decoder.hpp"
#include "test_util.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstring>
#include <limits>

using namespace bisondb;
using bisondb::test::hexToBytes;
using Catch::Matchers::ContainsSubstring;

namespace {

// Builds BSON documents byte-by-byte, independent of the encoder under test.
class DocBuilder {
  public:
    DocBuilder& element(uint8_t typeByte, std::string_view key,
                        const std::vector<uint8_t>& payload) {
        body_.push_back(typeByte);
        body_.insert(body_.end(), key.begin(), key.end());
        body_.push_back(0);
        body_.insert(body_.end(), payload.begin(), payload.end());
        return *this;
    }

    std::vector<uint8_t> finish() const {
        std::vector<uint8_t> out;
        uint32_t size = static_cast<uint32_t>(body_.size() + 5);
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>(size >> (8 * i)));
        }
        out.insert(out.end(), body_.begin(), body_.end());
        out.push_back(0);
        return out;
    }

  private:
    std::vector<uint8_t> body_;
};

std::vector<uint8_t> le32(uint32_t v) {
    return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
            static_cast<uint8_t>(v >> 24)};
}

std::vector<uint8_t> le64(uint64_t v) {
    std::vector<uint8_t> out;
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    return out;
}

std::vector<uint8_t> stringPayload(std::string_view s) {
    std::vector<uint8_t> out = le32(static_cast<uint32_t>(s.size() + 1));
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
    return out;
}

} // namespace

TEST_CASE("decode double", "[decoder]") {
    uint64_t bits;
    double d = 1.25;
    std::memcpy(&bits, &d, 8);
    Value v = decodeDocument(DocBuilder().element(0x01, "d", le64(bits)).finish());
    REQUIRE(v.asDocument().find("d")->get<double>() == 1.25);
}

TEST_CASE("decode double NaN survives", "[decoder]") {
    Value v = decodeDocument(DocBuilder().element(0x01, "d", le64(0x7FF8000000000000ULL)).finish());
    REQUIRE(std::isnan(v.asDocument().find("d")->get<double>()));
}

TEST_CASE("decode string", "[decoder]") {
    Value v = decodeDocument(DocBuilder().element(0x02, "s", stringPayload("hello")).finish());
    REQUIRE(v.asDocument().find("s")->get<std::string>() == "hello");
}

TEST_CASE("decode empty string and string with embedded NULs", "[decoder]") {
    Value v = decodeDocument(DocBuilder()
                                 .element(0x02, "e", stringPayload(""))
                                 .element(0x02, "n", stringPayload(std::string_view("a\0b", 3)))
                                 .finish());
    REQUIRE(v.asDocument().find("e")->get<std::string>().empty());
    REQUIRE(v.asDocument().find("n")->get<std::string>() == std::string("a\0b", 3));
}

TEST_CASE("decode nested document", "[decoder]") {
    std::vector<uint8_t> inner = DocBuilder().element(0x10, "i", le32(7)).finish();
    Value v = decodeDocument(DocBuilder().element(0x03, "sub", inner).finish());
    const Document& sub = v.asDocument().find("sub")->asDocument();
    REQUIRE(sub.find("i")->get<int32_t>() == 7);
}

TEST_CASE("decode array ignores stored index keys", "[decoder]") {
    // Keys deliberately wrong ("5", "9"): BSON readers use position, not key.
    std::vector<uint8_t> inner =
        DocBuilder().element(0x10, "5", le32(10)).element(0x10, "9", le32(20)).finish();
    Value v = decodeDocument(DocBuilder().element(0x04, "a", inner).finish());
    const Array& arr = v.asDocument().find("a")->asArray();
    REQUIRE(arr.size() == 2);
    REQUIRE(arr[0].get<int32_t>() == 10);
    REQUIRE(arr[1].get<int32_t>() == 20);
}

TEST_CASE("decode ObjectId", "[decoder]") {
    std::vector<uint8_t> oid = hexToBytes("56E1FC72E0C917E9C4714161");
    Value v = decodeDocument(DocBuilder().element(0x07, "_id", oid).finish());
    REQUIRE(v.asDocument().find("_id")->get<ObjectId>().toHex() == "56e1fc72e0c917e9c4714161");
}

TEST_CASE("decode bool", "[decoder]") {
    Value v =
        decodeDocument(DocBuilder().element(0x08, "t", {0x01}).element(0x08, "f", {0x00}).finish());
    REQUIRE(v.asDocument().find("t")->get<bool>() == true);
    REQUIRE(v.asDocument().find("f")->get<bool>() == false);
}

TEST_CASE("decode DateTime including negative epoch offsets", "[decoder]") {
    Value v = decodeDocument(
        DocBuilder()
            .element(0x09, "d", le64(static_cast<uint64_t>(int64_t{1356351330501})))
            .element(0x09, "neg", le64(static_cast<uint64_t>(int64_t{-284643869501})))
            .finish());
    REQUIRE(v.asDocument().find("d")->get<DateTime>().msSinceEpoch == 1356351330501);
    REQUIRE(v.asDocument().find("neg")->get<DateTime>().msSinceEpoch == -284643869501);
}

TEST_CASE("decode null", "[decoder]") {
    Value v = decodeDocument(DocBuilder().element(0x0A, "n", {}).finish());
    REQUIRE(v.asDocument().find("n")->isNull());
}

TEST_CASE("decode int32 extremes", "[decoder]") {
    Value v = decodeDocument(DocBuilder()
                                 .element(0x10, "min", le32(0x80000000U))
                                 .element(0x10, "max", le32(0x7FFFFFFFU))
                                 .finish());
    REQUIRE(v.asDocument().find("min")->get<int32_t>() == std::numeric_limits<int32_t>::min());
    REQUIRE(v.asDocument().find("max")->get<int32_t>() == std::numeric_limits<int32_t>::max());
}

TEST_CASE("decode int64 extremes", "[decoder]") {
    Value v = decodeDocument(DocBuilder()
                                 .element(0x12, "min", le64(0x8000000000000000ULL))
                                 .element(0x12, "max", le64(0x7FFFFFFFFFFFFFFFULL))
                                 .finish());
    REQUIRE(v.asDocument().find("min")->get<int64_t>() == std::numeric_limits<int64_t>::min());
    REQUIRE(v.asDocument().find("max")->get<int64_t>() == std::numeric_limits<int64_t>::max());
}

TEST_CASE("decode Decimal128 raw bytes", "[decoder]") {
    std::vector<uint8_t> raw = hexToBytes("01000000000000000000000000003040");
    Value v = decodeDocument(DocBuilder().element(0x13, "d", raw).finish());
    const Decimal128& d = v.asDocument().find("d")->get<Decimal128>();
    REQUIRE(std::vector<uint8_t>(d.bytes.begin(), d.bytes.end()) == raw);
}

TEST_CASE("decodeOne streams concatenated documents", "[decoder]") {
    std::vector<uint8_t> first = DocBuilder().element(0x10, "a", le32(1)).finish();
    std::vector<uint8_t> second = DocBuilder().element(0x10, "b", le32(2)).finish();
    std::vector<uint8_t> both = first;
    both.insert(both.end(), second.begin(), second.end());

    DecodeResult r1 = decodeOne(both);
    REQUIRE(r1.bytesConsumed == first.size());
    REQUIRE(r1.document.asDocument().find("a")->get<int32_t>() == 1);

    DecodeResult r2 = decodeOne(std::span<const uint8_t>(both).subspan(r1.bytesConsumed));
    REQUIRE(r2.bytesConsumed == second.size());
    REQUIRE(r2.document.asDocument().find("b")->get<int32_t>() == 2);
}

// ---- rejection cases --------------------------------------------------------

TEST_CASE("decode rejects empty input", "[decoder]") {
    REQUIRE_THROWS_AS(decodeDocument(std::span<const uint8_t>{}), BsonParseError);
}

TEST_CASE("decode rejects size below minimum", "[decoder]") {
    REQUIRE_THROWS_MATCHES(decodeDocument(hexToBytes("0400000000")), BsonParseError,
                           Catch::Matchers::MessageMatches(ContainsSubstring("size must be >= 5")));
}

TEST_CASE("decode rejects size larger than the input", "[decoder]") {
    std::vector<uint8_t> doc = DocBuilder().element(0x10, "a", le32(1)).finish();
    doc.pop_back();
    REQUIRE_THROWS_MATCHES(
        decodeDocument(doc), BsonParseError,
        Catch::Matchers::MessageMatches(ContainsSubstring("exceeds available bytes")));
}

TEST_CASE("decodeDocument rejects trailing bytes but decodeOne tolerates them", "[decoder]") {
    std::vector<uint8_t> doc = DocBuilder().element(0x10, "a", le32(1)).finish();
    std::size_t cleanSize = doc.size();
    doc.push_back(0xAB);
    REQUIRE_THROWS_MATCHES(decodeDocument(doc), BsonParseError,
                           Catch::Matchers::MessageMatches(ContainsSubstring("trailing bytes")));
    REQUIRE(decodeOne(doc).bytesConsumed == cleanSize);
}

TEST_CASE("decode rejects unknown type bytes", "[decoder]") {
    std::vector<uint8_t> doc = DocBuilder().element(0xEE, "a", le32(1)).finish();
    REQUIRE_THROWS_MATCHES(
        decodeDocument(doc), BsonParseError,
        Catch::Matchers::MessageMatches(ContainsSubstring("unknown BSON type byte 0xee")));
}

TEST_CASE("decode rejects bool bytes other than 0 and 1", "[decoder]") {
    REQUIRE_THROWS_MATCHES(
        decodeDocument(DocBuilder().element(0x08, "b", {0x02}).finish()), BsonParseError,
        Catch::Matchers::MessageMatches(ContainsSubstring("bool byte must be 0 or 1")));
    REQUIRE_THROWS_AS(decodeDocument(DocBuilder().element(0x08, "b", {0xFF}).finish()),
                      BsonParseError);
}

TEST_CASE("decode rejects bad string lengths", "[decoder]") {
    SECTION("length 0") {
        auto payload = le32(0);
        payload.push_back(0);
        REQUIRE_THROWS_MATCHES(
            decodeDocument(DocBuilder().element(0x02, "s", payload).finish()), BsonParseError,
            Catch::Matchers::MessageMatches(ContainsSubstring("string length must be >= 1")));
    }
    SECTION("length -1") {
        auto payload = le32(0xFFFFFFFFU);
        payload.push_back(0);
        REQUIRE_THROWS_AS(decodeDocument(DocBuilder().element(0x02, "s", payload).finish()),
                          BsonParseError);
    }
    SECTION("length eats the document terminator") {
        std::vector<uint8_t> doc = DocBuilder().element(0x02, "s", stringPayload("b")).finish();
        doc[7] = 3; // declared string length now covers the document terminator
        REQUIRE_THROWS_AS(decodeDocument(doc), BsonParseError);
    }
    SECTION("length extends past the document") {
        std::vector<uint8_t> doc = DocBuilder().element(0x02, "s", stringPayload("b")).finish();
        doc[7] = 100;
        REQUIRE_THROWS_MATCHES(
            decodeDocument(doc), BsonParseError,
            Catch::Matchers::MessageMatches(ContainsSubstring("extends past end of document")));
    }
    SECTION("string not NUL-terminated") {
        std::vector<uint8_t> doc = DocBuilder().element(0x02, "s", stringPayload("ab")).finish();
        doc[13] = 0x21; // overwrite the string's own terminator
        REQUIRE_THROWS_MATCHES(
            decodeDocument(doc), BsonParseError,
            Catch::Matchers::MessageMatches(ContainsSubstring("not NUL-terminated")));
    }
}

TEST_CASE("decode rejects invalid UTF-8 in string values and keys", "[decoder]") {
    SECTION("truncated two-byte sequence in value") {
        std::vector<uint8_t> payload = {0x02, 0x00, 0x00, 0x00, 0xE9, 0x00};
        REQUIRE_THROWS_MATCHES(
            decodeDocument(DocBuilder().element(0x02, "s", payload).finish()), BsonParseError,
            Catch::Matchers::MessageMatches(ContainsSubstring("not valid UTF-8")));
    }
    SECTION("overlong encoding in key") {
        std::vector<uint8_t> doc =
            DocBuilder().element(0x10, std::string_view("\xC0\x80xy"), le32(1)).finish();
        REQUIRE_THROWS_MATCHES(
            decodeDocument(doc), BsonParseError,
            Catch::Matchers::MessageMatches(ContainsSubstring("key is not valid UTF-8")));
    }
}

TEST_CASE("decode rejects terminator irregularities", "[decoder]") {
    SECTION("missing terminator") {
        REQUIRE_THROWS_MATCHES(
            decodeDocument(hexToBytes("0500000001")), BsonParseError,
            Catch::Matchers::MessageMatches(ContainsSubstring("missing document terminator")));
    }
    SECTION("premature terminator before declared size") {
        REQUIRE_THROWS_MATCHES(
            decodeDocument(hexToBytes("060000000000")), BsonParseError,
            Catch::Matchers::MessageMatches(ContainsSubstring("premature document terminator")));
    }
    SECTION("unterminated cstring key") {
        REQUIRE_THROWS_MATCHES(
            decodeDocument(hexToBytes("07000000106161")), BsonParseError,
            Catch::Matchers::MessageMatches(ContainsSubstring("unterminated cstring key")));
    }
}

TEST_CASE("decode rejects element values crossing the document boundary", "[decoder]") {
    // Subdocument whose declared size eats the outer terminator (from the
    // official corpus).
    REQUIRE_THROWS_AS(
        decodeDocument(hexToBytes("1800000003666F6F000F0000001062617200FFFFFF7F0000")),
        BsonParseError);
    // Subdocument length too short: leaks its terminator into the outer doc.
    REQUIRE_THROWS_AS(decodeDocument(hexToBytes("1500000003666F6F000A0000000862617200010000")),
                      BsonParseError);
}

TEST_CASE("decode rejects nesting deeper than 200", "[decoder]") {
    std::vector<uint8_t> doc = DocBuilder().finish(); // {}
    for (int i = 0; i < 201; ++i) {
        doc = DocBuilder().element(0x03, "a", doc).finish();
    }
    REQUIRE_THROWS_MATCHES(
        decodeDocument(doc), BsonParseError,
        Catch::Matchers::MessageMatches(ContainsSubstring("nesting depth exceeds limit")));

    // 200 levels of nesting are still fine.
    std::vector<uint8_t> ok = DocBuilder().finish();
    for (int i = 0; i < 199; ++i) {
        ok = DocBuilder().element(0x03, "a", ok).finish();
    }
    REQUIRE_NOTHROW(decodeDocument(ok));
}

TEST_CASE("BsonParseError reports the failing byte offset", "[decoder]") {
    std::vector<uint8_t> doc = DocBuilder().element(0xEE, "a", le32(1)).finish();
    try {
        decodeDocument(doc);
        FAIL("expected BsonParseError");
    } catch (const BsonParseError& e) {
        REQUIRE(e.offset() == 4); // the type byte of the first element
        REQUIRE_THAT(e.what(), ContainsSubstring("at byte 4"));
    }
}

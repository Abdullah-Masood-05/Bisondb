// Runs the official BSON corpus cases (github.com/mongodb/specifications,
// source/bson-corpus) for the 11 supported types. Corpus JSON files live in
// tests/corpus/; see tests/corpus/download.ps1. Each file's own JSON is parsed
// with this project's JSON parser.

#include "core/bson_decoder.hpp"
#include "core/bson_encoder.hpp"
#include "core/json_parser.hpp"
#include "test_util.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include <vector>

using namespace bisondb;
using bisondb::test::hexToBytes;
using bisondb::test::readFileText;

namespace {

const std::vector<std::string> kCorpusFiles = {
    "double.json", "string.json",  "document.json",     "array.json",
    "oid.json",    "boolean.json", "datetime.json",     "null.json",
    "int32.json",  "int64.json",   "decimal128-1.json",
};

std::filesystem::path corpusDir() {
    return std::filesystem::path(BISONDB_TESTS_DIR) / "corpus";
}

const std::string& stringField(const Document& doc, std::string_view key) {
    const Value* v = doc.find(key);
    REQUIRE(v != nullptr);
    return v->get<std::string>();
}

} // namespace

TEST_CASE("official BSON corpus", "[corpus]") {
    if (!std::filesystem::exists(corpusDir())) {
        SKIP("tests/corpus/ not present - run tests/corpus/download.ps1 to fetch it");
    }

    std::size_t validCases = 0;
    std::size_t decodeErrorCases = 0;
    std::size_t skippedFiles = 0;

    for (const std::string& name : kCorpusFiles) {
        std::filesystem::path path = corpusDir() / name;
        if (!std::filesystem::exists(path)) {
            ++skippedFiles;
            WARN("corpus file missing, skipping: " << name);
            continue;
        }
        INFO("corpus file: " << name);
        Value corpus = parseJson(readFileText(path.string()));
        const Document& root = corpus.asDocument();

        if (const Value* valid = root.find("valid")) {
            for (const Value& entry : valid->asArray()) {
                const Document& c = entry.asDocument();
                INFO("valid case: " << stringField(c, "description"));
                std::vector<uint8_t> canonical = hexToBytes(stringField(c, "canonical_bson"));

                // Canonical bytes must decode and re-encode to identical bytes.
                Value decoded = decodeDocument(canonical);
                REQUIRE(encodeDocument(decoded) == canonical);

                // Degenerate bytes must decode, then re-encode to canonical.
                if (const Value* degenerate = c.find("degenerate_bson")) {
                    Value dv = decodeDocument(hexToBytes(degenerate->get<std::string>()));
                    REQUIRE(encodeDocument(dv) == canonical);
                }
                ++validCases;
            }
        }

        if (const Value* errors = root.find("decodeErrors")) {
            for (const Value& entry : errors->asArray()) {
                const Document& c = entry.asDocument();
                INFO("decodeErrors case: " << stringField(c, "description"));
                std::vector<uint8_t> bytes = hexToBytes(stringField(c, "bson"));
                REQUIRE_THROWS_AS(decodeDocument(bytes), BsonParseError);
                ++decodeErrorCases;
            }
        }
    }

    INFO("corpus summary: " << validCases << " valid cases, " << decodeErrorCases
                            << " decode-error cases, " << skippedFiles << " files skipped");
    REQUIRE(skippedFiles < kCorpusFiles.size()); // at least one file must have run
    CHECK(validCases > 0);
    CHECK(decodeErrorCases > 0);
}

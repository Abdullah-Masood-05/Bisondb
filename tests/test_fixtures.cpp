// Round-trips every document in tests/fixtures/*.bson (e.g. real mongodump
// output) and asserts byte-identical re-encoding. Runs as a no-op when the
// directory is absent or empty, so dropping new .bson files in is enough to
// extend coverage.

#include "core/bson_decoder.hpp"
#include "core/bson_encoder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <span>
#include <vector>

using namespace bisondb;
using bisondb::test::readFileBytes;

TEST_CASE("fixture .bson files re-encode byte-identically", "[fixtures]") {
    std::filesystem::path dir = std::filesystem::path(BISONDB_TESTS_DIR) / "fixtures";
    if (!std::filesystem::exists(dir)) {
        SKIP("tests/fixtures/ not present");
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".bson") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        SKIP("no .bson fixtures present");
    }

    for (const auto& file : files) {
        INFO("fixture: " << file.filename().string());
        std::vector<uint8_t> data = readFileBytes(file.string());
        std::span<const uint8_t> rest(data);
        std::size_t docIndex = 0;
        while (!rest.empty()) {
            INFO("document #" << docIndex);
            DecodeResult res = decodeOne(rest);
            std::vector<uint8_t> original(
                rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(res.bytesConsumed));
            REQUIRE(encodeDocument(res.document) == original);
            rest = rest.subspan(res.bytesConsumed);
            ++docIndex;
        }
        REQUIRE(docIndex > 0);
    }
}

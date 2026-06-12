#include "core/version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("version string is non-empty", "[sanity]") {
    REQUIRE_FALSE(bisondb::version().empty());
}

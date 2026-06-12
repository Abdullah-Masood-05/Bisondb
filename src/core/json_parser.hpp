#pragma once

#include "core/value.hpp"

#include <cstddef>
#include <string_view>

namespace bisondb {

struct JsonParseOptions {
    // Relaxed mode (for the interactive shell) additionally accepts unquoted
    // object keys matching [A-Za-z_$][A-Za-z0-9_$]*, single-quoted strings
    // (same escape set), and trailing commas in objects/arrays. Strict mode
    // is exactly RFC 8259.
    bool relaxed = false;
};

// Parses one complete JSON text (RFC 8259) into a Value, folding MongoDB
// Extended JSON v2 wrappers ($oid, $date, $numberInt, $numberLong,
// $numberDouble, $numberDecimal) into their typed equivalents. Anything after
// the top-level value other than whitespace is an error. Throws
// JsonParseError with 1-based line/column on malformed input.
Value parseJson(std::string_view text, const JsonParseOptions& options = {});

// Streaming variant: parses a single top-level value from the front of
// `text` (after leading whitespace) and reports the number of bytes consumed,
// so JSON Lines / concatenated documents can be read in a loop.
Value parseJsonOne(std::string_view text, std::size_t& bytesConsumed,
                   const JsonParseOptions& options = {});

} // namespace bisondb

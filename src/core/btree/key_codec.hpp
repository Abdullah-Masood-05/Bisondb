#pragma once

#include "core/error.hpp"
#include "core/value.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace bisondb::btree {

class KeyTooLong : public Error {
  public:
    using Error::Error;
};

// Raised for values that cannot participate in an index: NaN doubles (BisonDB
// skips them instead of giving them a sort position like MongoDB does) and
// types without a defined key encoding (Document, Array, Decimal128).
class KeyNotIndexable : public Error {
  public:
    using Error::Error;
};

constexpr std::size_t kMaxEncodedKeyLength = 512;

// Encodes a Value so unsigned lexicographic byte comparison of the encodings
// matches the desired value ordering. Cross-type order (MongoDB-like):
//   Null < Numbers < String < ObjectId < Bool < DateTime
// Numbers (Int32/Int64/Double) are normalized to double: integers above 2^53
// lose precision and may compare equal to neighbouring values.
std::vector<uint8_t> encodeKey(const Value& v);

// True when encodeKey() would succeed (indexable type, not NaN). Length is
// not checked here.
bool isIndexableType(const Value& v);

// Reference comparator over raw Values implementing exactly the order that
// encodeKey() encodes: negative/zero/positive like memcmp, nullopt when the
// values are not comparable under the index ordering (either not indexable).
std::optional<int> compareIndexOrder(const Value& a, const Value& b);

// Secondary-index composite key: encoded field value, a 0x00 separator, then
// the 12 ObjectId bytes — making every stored key unique while preserving
// (fieldValue, oid) ordering.
std::vector<uint8_t> composeIndexKey(const std::vector<uint8_t>& encodedField, const ObjectId& oid);

// Recovers the ObjectId from a composite key (the trailing 12 bytes).
ObjectId oidFromCompositeKey(const std::vector<uint8_t>& key);

} // namespace bisondb::btree

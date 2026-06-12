#include "core/btree/key_codec.hpp"

#include <bit>
#include <cmath>
#include <cstring>

namespace bisondb::btree {

namespace {

// Type-class tag bytes; the byte values define the cross-type sort order.
constexpr uint8_t kTagNull = 0x05;
constexpr uint8_t kTagNumber = 0x10;
constexpr uint8_t kTagString = 0x20;
constexpr uint8_t kTagObjectId = 0x30;
constexpr uint8_t kTagBool = 0x40;
constexpr uint8_t kTagDateTime = 0x50;

void appendBigEndian64(std::vector<uint8_t>& out, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(v >> shift));
    }
}

// Maps double bits to a uint64 whose unsigned order equals numeric order:
// negative numbers have the sign bit set, so flipping all bits reverses their
// (descending) bit order; non-negatives just need the sign bit flipped to
// sort above them.
uint64_t orderedDoubleBits(double d) {
    if (d == 0.0) {
        d = 0.0; // normalize -0.0 to +0.0
    }
    uint64_t bits = std::bit_cast<uint64_t>(d);
    if (bits & 0x8000'0000'0000'0000ULL) {
        return ~bits;
    }
    return bits | 0x8000'0000'0000'0000ULL;
}

std::optional<double> numericValue(const Value& v) {
    switch (v.type()) {
    case Type::Int32: return static_cast<double>(v.get<int32_t>());
    case Type::Int64: return static_cast<double>(v.get<int64_t>());
    case Type::Double: return v.get<double>();
    default: return std::nullopt;
    }
}

// Class rank used by the reference comparator; mirrors the tag bytes.
std::optional<int> typeClassRank(const Value& v) {
    switch (v.type()) {
    case Type::Null: return 0;
    case Type::Int32:
    case Type::Int64:
    case Type::Double: return 1;
    case Type::String: return 2;
    case Type::ObjectId: return 3;
    case Type::Bool: return 4;
    case Type::DateTime: return 5;
    default: return std::nullopt;
    }
}

} // namespace

bool isIndexableType(const Value& v) {
    if (!typeClassRank(v).has_value()) {
        return false;
    }
    if (v.is<double>() && std::isnan(v.get<double>())) {
        return false;
    }
    return true;
}

std::vector<uint8_t> encodeKey(const Value& v) {
    std::vector<uint8_t> out;
    switch (v.type()) {
    case Type::Null: out.push_back(kTagNull); break;
    case Type::Int32:
    case Type::Int64:
    case Type::Double: {
        double d = *numericValue(v);
        if (std::isnan(d)) {
            throw KeyNotIndexable("NaN is not indexable");
        }
        out.push_back(kTagNumber);
        appendBigEndian64(out, orderedDoubleBits(d));
        break;
    }
    case Type::String: {
        const std::string& s = v.get<std::string>();
        out.push_back(kTagString);
        for (char c : s) {
            uint8_t b = static_cast<uint8_t>(c);
            out.push_back(b);
            if (b == 0x00) {
                out.push_back(0xFF); // escape embedded NUL
            }
        }
        out.push_back(0x00); // terminator: 0x00 0x00
        out.push_back(0x00);
        break;
    }
    case Type::ObjectId: {
        out.push_back(kTagObjectId);
        const auto& bytes = v.get<ObjectId>().bytes;
        out.insert(out.end(), bytes.begin(), bytes.end());
        break;
    }
    case Type::Bool:
        out.push_back(kTagBool);
        out.push_back(v.get<bool>() ? 1 : 0);
        break;
    case Type::DateTime: {
        out.push_back(kTagDateTime);
        uint64_t biased = static_cast<uint64_t>(v.get<DateTime>().msSinceEpoch) +
                          0x8000'0000'0000'0000ULL;
        appendBigEndian64(out, biased);
        break;
    }
    default:
        throw KeyNotIndexable(std::string("type ") + std::string(typeName(v.type())) +
                              " is not indexable");
    }
    if (out.size() > kMaxEncodedKeyLength) {
        throw KeyTooLong("encoded key is " + std::to_string(out.size()) +
                         " bytes (max " + std::to_string(kMaxEncodedKeyLength) + ")");
    }
    return out;
}

std::optional<int> compareIndexOrder(const Value& a, const Value& b) {
    if (!isIndexableType(a) || !isIndexableType(b)) {
        return std::nullopt;
    }
    int ra = *typeClassRank(a);
    int rb = *typeClassRank(b);
    if (ra != rb) {
        return ra < rb ? -1 : 1;
    }
    switch (ra) {
    case 0: return 0; // Null == Null
    case 1: {
        double da = *numericValue(a);
        double db = *numericValue(b);
        if (da == 0.0) {
            da = 0.0;
        }
        if (db == 0.0) {
            db = 0.0;
        }
        if (da < db) {
            return -1;
        }
        if (da > db) {
            return 1;
        }
        return 0;
    }
    case 2: {
        int c = a.get<std::string>().compare(b.get<std::string>());
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    case 3: {
        int c = std::memcmp(a.get<ObjectId>().bytes.data(), b.get<ObjectId>().bytes.data(), 12);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    case 4: {
        int ia = a.get<bool>() ? 1 : 0;
        int ib = b.get<bool>() ? 1 : 0;
        return ia - ib;
    }
    default: {
        int64_t ta = a.get<DateTime>().msSinceEpoch;
        int64_t tb = b.get<DateTime>().msSinceEpoch;
        return ta < tb ? -1 : (ta > tb ? 1 : 0);
    }
    }
}

std::vector<uint8_t> composeIndexKey(const std::vector<uint8_t>& encodedField,
                                     const ObjectId& oid) {
    std::vector<uint8_t> out;
    out.reserve(encodedField.size() + 13);
    out.insert(out.end(), encodedField.begin(), encodedField.end());
    out.push_back(0x00);
    out.insert(out.end(), oid.bytes.begin(), oid.bytes.end());
    return out;
}

ObjectId oidFromCompositeKey(const std::vector<uint8_t>& key) {
    if (key.size() < 13) {
        throw KeyNotIndexable("composite key too short to contain an ObjectId");
    }
    ObjectId oid;
    std::memcpy(oid.bytes.data(), key.data() + key.size() - 12, 12);
    return oid;
}

} // namespace bisondb::btree

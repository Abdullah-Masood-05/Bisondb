#pragma once

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bisondb::test {

inline std::vector<uint8_t> hexToBytes(std::string_view hex) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        throw std::runtime_error("bad hex digit in test data");
    };
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("odd-length hex string in test data");
    }
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

inline std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

inline std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open test file: " + path);
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

inline std::string readFileText(const std::string& path) {
    std::vector<uint8_t> bytes = readFileBytes(path);
    std::string text(bytes.begin(), bytes.end());
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3); // strip UTF-8 BOM
    }
    return text;
}

} // namespace bisondb::test

#pragma once

#include "core/btree/pager.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace bisondb::btree {

// Slotted-page node layout (both node kinds), all integers little-endian:
//   0       u8  nodeType (1 = leaf, 2 = internal)
//   1       u8  unused
//   2..3    u16 cellCount
//   4..5    u16 freeSpaceOffset (lowest cell start; cells grow downward)
//   6..7    u16 unused
//   8..11   u32 rightSibling (leaf) / rightmost child page (internal)
//   12..    slot array: u16 cell offsets, sorted by key
// Leaf cell:     u16 keyLen | key | u16 valLen | value
// Internal cell: u16 keyLen | key | u32 childPageId
//   (internal cell i's child holds keys < key i; keys >= the last separator
//    live under the header's rightmost child)
//
// Node is a view + editor over a caller-owned page buffer.
class Node {
  public:
    static constexpr uint8_t kLeaf = 1;
    static constexpr uint8_t kInternal = 2;
    static constexpr std::size_t kHeaderSize = 12;

    Node(uint8_t* buf, uint32_t pageSize) : buf_(buf), pageSize_(pageSize) {}

    void init(uint8_t type) {
        std::memset(buf_, 0, pageSize_);
        buf_[0] = type;
        setCellCount(0);
        setFreeOff(static_cast<uint16_t>(pageSize_));
        setRight(0);
    }

    uint8_t type() const { return buf_[0]; }
    bool isLeaf() const { return type() == kLeaf; }

    uint16_t cellCount() const { return loadU16(2); }
    void setCellCount(uint16_t n) { storeU16(2, n); }

    uint16_t freeOff() const { return loadU16(4); }
    void setFreeOff(uint16_t v) { storeU16(4, v); }

    // Leaf: right sibling page (0 = none). Internal: rightmost child page.
    PageId right() const { return loadU32(8); }
    void setRight(PageId id) { storeU32(8, id); }

    uint16_t slot(uint16_t i) const { return loadU16(kHeaderSize + 2u * i); }

    std::span<const uint8_t> keyAt(uint16_t i) const {
        std::size_t off = slot(i);
        uint16_t keyLen = loadU16(off);
        return {buf_ + off + 2, keyLen};
    }

    std::span<const uint8_t> valueAt(uint16_t i) const { // leaf only
        std::size_t off = slot(i);
        uint16_t keyLen = loadU16(off);
        uint16_t valLen = loadU16(off + 2 + keyLen);
        return {buf_ + off + 4 + keyLen, valLen};
    }

    PageId childAt(uint16_t i) const { // internal only
        std::size_t off = slot(i);
        uint16_t keyLen = loadU16(off);
        return loadU32(off + 2 + keyLen);
    }

    void setChildAt(uint16_t i, PageId id) { // internal only
        std::size_t off = slot(i);
        uint16_t keyLen = loadU16(off);
        storeU32(off + 2 + keyLen, id);
    }

    // First index whose key >= `key` (cellCount() if none).
    uint16_t lowerBound(std::span<const uint8_t> key) const {
        uint16_t lo = 0;
        uint16_t hi = cellCount();
        while (lo < hi) {
            uint16_t mid = static_cast<uint16_t>((lo + hi) / 2);
            if (compareKeys(keyAt(mid), key) < 0) {
                lo = static_cast<uint16_t>(mid + 1);
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    // First index whose key > `key` (cellCount() if none).
    uint16_t upperBound(std::span<const uint8_t> key) const {
        uint16_t lo = 0;
        uint16_t hi = cellCount();
        while (lo < hi) {
            uint16_t mid = static_cast<uint16_t>((lo + hi) / 2);
            if (compareKeys(keyAt(mid), key) <= 0) {
                lo = static_cast<uint16_t>(mid + 1);
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    static int compareKeys(std::span<const uint8_t> a, std::span<const uint8_t> b) {
        std::size_t n = a.size() < b.size() ? a.size() : b.size();
        int c = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
        if (c != 0) {
            return c;
        }
        if (a.size() == b.size()) {
            return 0;
        }
        return a.size() < b.size() ? -1 : 1;
    }

    static std::size_t leafCellSize(std::size_t keyLen, std::size_t valLen) {
        return 4 + keyLen + valLen;
    }
    static std::size_t internalCellSize(std::size_t keyLen) { return 6 + keyLen; }

    std::size_t freeSpace() const {
        std::size_t slotsEnd = kHeaderSize + 2u * cellCount();
        return freeOff() - slotsEnd;
    }

    // Attempts to place a cell at slot index i; returns false when the page
    // is full even after compaction (caller must split).
    bool insertLeafCell(uint16_t i, std::span<const uint8_t> key, std::span<const uint8_t> val) {
        std::size_t size = leafCellSize(key.size(), val.size());
        if (!ensureRoom(size)) {
            return false;
        }
        uint16_t off = static_cast<uint16_t>(freeOff() - size);
        storeU16(off, static_cast<uint16_t>(key.size()));
        std::memcpy(buf_ + off + 2, key.data(), key.size());
        storeU16(off + 2 + key.size(), static_cast<uint16_t>(val.size()));
        if (!val.empty()) {
            std::memcpy(buf_ + off + 4 + key.size(), val.data(), val.size());
        }
        setFreeOff(off);
        insertSlot(i, off);
        return true;
    }

    bool insertInternalCell(uint16_t i, std::span<const uint8_t> key, PageId child) {
        std::size_t size = internalCellSize(key.size());
        if (!ensureRoom(size)) {
            return false;
        }
        uint16_t off = static_cast<uint16_t>(freeOff() - size);
        storeU16(off, static_cast<uint16_t>(key.size()));
        std::memcpy(buf_ + off + 2, key.data(), key.size());
        storeU32(off + 2 + key.size(), child);
        setFreeOff(off);
        insertSlot(i, off);
        return true;
    }

    // Removes the slot; the cell bytes become a hole reclaimed by compact().
    void removeCell(uint16_t i) {
        uint16_t n = cellCount();
        std::memmove(buf_ + kHeaderSize + 2u * i, buf_ + kHeaderSize + 2u * (i + 1),
                     2u * (n - i - 1));
        setCellCount(static_cast<uint16_t>(n - 1));
    }

  private:
    void insertSlot(uint16_t i, uint16_t off) {
        uint16_t n = cellCount();
        std::memmove(buf_ + kHeaderSize + 2u * (i + 1), buf_ + kHeaderSize + 2u * i, 2u * (n - i));
        storeU16(kHeaderSize + 2u * i, off);
        setCellCount(static_cast<uint16_t>(n + 1));
    }

    bool ensureRoom(std::size_t cellSize) {
        if (freeSpace() >= cellSize + 2) {
            return true;
        }
        compact();
        return freeSpace() >= cellSize + 2;
    }

    // Rewrites all live cells tightly against the page end, reclaiming holes
    // left by removeCell().
    void compact() {
        uint16_t n = cellCount();
        std::vector<uint8_t> cells;
        std::vector<uint16_t> sizes(n);
        cells.reserve(pageSize_);
        for (uint16_t i = 0; i < n; ++i) {
            std::size_t off = slot(i);
            uint16_t keyLen = loadU16(off);
            std::size_t size = isLeaf() ? leafCellSize(keyLen, loadU16(off + 2 + keyLen))
                                        : internalCellSize(keyLen);
            sizes[i] = static_cast<uint16_t>(size);
            cells.insert(cells.end(), buf_ + off, buf_ + off + size);
        }
        uint16_t off = static_cast<uint16_t>(pageSize_);
        std::size_t src = 0;
        for (uint16_t i = 0; i < n; ++i) {
            off = static_cast<uint16_t>(off - sizes[i]);
            std::memcpy(buf_ + off, cells.data() + src, sizes[i]);
            storeU16(kHeaderSize + 2u * i, off);
            src += sizes[i];
        }
        setFreeOff(off);
    }

    uint16_t loadU16(std::size_t off) const {
        return static_cast<uint16_t>(buf_[off] | (buf_[off + 1] << 8));
    }
    void storeU16(std::size_t off, uint16_t v) {
        buf_[off] = static_cast<uint8_t>(v);
        buf_[off + 1] = static_cast<uint8_t>(v >> 8);
    }
    uint32_t loadU32(std::size_t off) const {
        return static_cast<uint32_t>(buf_[off]) | (static_cast<uint32_t>(buf_[off + 1]) << 8) |
               (static_cast<uint32_t>(buf_[off + 2]) << 16) |
               (static_cast<uint32_t>(buf_[off + 3]) << 24);
    }
    void storeU32(std::size_t off, uint32_t v) {
        buf_[off] = static_cast<uint8_t>(v);
        buf_[off + 1] = static_cast<uint8_t>(v >> 8);
        buf_[off + 2] = static_cast<uint8_t>(v >> 16);
        buf_[off + 3] = static_cast<uint8_t>(v >> 24);
    }

    uint8_t* buf_;
    uint32_t pageSize_;
};

} // namespace bisondb::btree

#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <cstdio>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace bisondb::btree {

class PagerError : public Error {
  public:
    using Error::Error;
};

// Thrown when an index file was not closed cleanly; the caller is expected to
// discard and rebuild it.
class RebuildRequired : public Error {
  public:
    using Error::Error;
};

using PageId = uint32_t;

// Fixed-size pages over a single file. Page 0 holds the file header (magic,
// page size, root page, free list, clean flag); data pages start at 1. Reads
// and writes go through an LRU write-back cache; no mmap, so behavior is
// identical on Windows and Linux.
class Pager {
  public:
    static constexpr uint32_t kDefaultPageSize = 4096;
    static constexpr std::size_t kDefaultCacheCapacity = 256;
    static constexpr uint32_t kMinPageSize = 128;

    // Opens or creates the file. An existing file must match `pageSize` and
    // carry the BSNI magic. wasCleanOnOpen() reports the persisted clean flag.
    explicit Pager(const std::string& path, uint32_t pageSize = kDefaultPageSize,
                   std::size_t cacheCapacity = kDefaultCacheCapacity);
    ~Pager();

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    uint32_t pageSize() const noexcept { return pageSize_; }
    uint32_t pageCount() const noexcept { return pageCount_; }
    bool wasCleanOnOpen() const noexcept { return wasClean_; }

    PageId rootPage() const noexcept { return rootPage_; }
    void setRootPage(PageId id);

    // Allocates a page (reusing the free list when possible). The new page's
    // contents are unspecified; the caller must fully initialize it.
    PageId allocPage();
    void freePage(PageId id);

    // Whole-page copy in/out. `buf` must hold pageSize() bytes. Page ids must
    // be in [1, pageCount); page 0 is managed internally.
    void readPage(PageId id, uint8_t* buf);
    void writePage(PageId id, const uint8_t* buf);

    // Writes all dirty pages and the header, fsyncs, and marks the file clean.
    void flushAll();

  private:
    struct CacheEntry {
        PageId id;
        std::vector<uint8_t> data;
        bool dirty;
    };

    void checkDataPageId(PageId id) const;
    void ensureUncleanOnDisk();
    void writeHeaderToDisk(uint8_t cleanFlag);
    void filePut(PageId id, const uint8_t* buf);
    void fileGet(PageId id, uint8_t* buf);
    void syncToDisk();
    CacheEntry& cacheFetch(PageId id, bool loadFromFile);
    void evictIfNeeded();

    std::string path_;
    std::FILE* file_ = nullptr;
    uint32_t pageSize_;
    std::size_t cacheCapacity_;

    uint32_t pageCount_ = 1;
    PageId rootPage_ = 0;
    PageId freeListHead_ = 0;
    bool wasClean_ = true;
    bool dirtyMarked_ = false; // cleanFlag=0 already persisted this session

    std::list<CacheEntry> lru_; // front = most recently used
    std::unordered_map<PageId, std::list<CacheEntry>::iterator> cacheMap_;
};

} // namespace bisondb::btree

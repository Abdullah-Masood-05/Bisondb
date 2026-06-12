#include "core/btree/pager.hpp"

#include "core/platform.hpp"

#include <cstring>

#if defined(BISONDB_PLATFORM_WINDOWS)
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace bisondb::btree {

namespace {

constexpr char kMagic[4] = {'B', 'S', 'N', 'I'};
constexpr uint32_t kVersion = 1;

// Header layout in page 0 (all little-endian):
//   0  magic "BSNI"
//   4  u32 version
//   8  u32 pageSize
//  12  u32 rootPageId
//  16  u32 freeListHead
//  20  u32 pageCount
//  24  u8  cleanFlag
//  25+ reserved (zero)
constexpr std::size_t kOffVersion = 4;
constexpr std::size_t kOffPageSize = 8;
constexpr std::size_t kOffRoot = 12;
constexpr std::size_t kOffFreeList = 16;
constexpr std::size_t kOffPageCount = 20;
constexpr std::size_t kOffClean = 24;

uint32_t loadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void storeU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void fsyncFile(std::FILE* f) {
    if (std::fflush(f) != 0) {
        throw PagerError("fflush failed");
    }
#if defined(BISONDB_PLATFORM_WINDOWS)
    if (_commit(_fileno(f)) != 0) {
        throw PagerError("_commit failed");
    }
#else
    if (fsync(fileno(f)) != 0) {
        throw PagerError("fsync failed");
    }
#endif
}

} // namespace

Pager::Pager(const std::string& path, uint32_t pageSize, std::size_t cacheCapacity)
    : path_(path), pageSize_(pageSize), cacheCapacity_(cacheCapacity) {
    if (pageSize_ < kMinPageSize) {
        throw PagerError("page size must be at least " + std::to_string(kMinPageSize));
    }
    file_ = std::fopen(path.c_str(), "r+b");
    bool created = false;
    if (file_ == nullptr) {
        file_ = std::fopen(path.c_str(), "w+b");
        created = true;
    }
    if (file_ == nullptr) {
        throw PagerError("cannot open pager file: " + path);
    }

    if (!created) {
        std::fseek(file_, 0, SEEK_END);
        if (std::ftell(file_) == 0) {
            created = true;
        }
    }

    if (created) {
        writeHeaderToDisk(1);
        return;
    }

    std::vector<uint8_t> header(pageSize_);
    std::fseek(file_, 0, SEEK_SET);
    // The header is read with the caller's page size; validate before trusting
    // any field beyond the fixed prefix.
    std::size_t got = std::fread(header.data(), 1, header.size(), file_);
    if (got < 32 || std::memcmp(header.data(), kMagic, 4) != 0) {
        std::fclose(file_);
        file_ = nullptr;
        throw PagerError("not a BisonDB index file: " + path);
    }
    if (loadU32(header.data() + kOffVersion) != kVersion) {
        std::fclose(file_);
        file_ = nullptr;
        throw PagerError("unsupported index file version");
    }
    if (loadU32(header.data() + kOffPageSize) != pageSize_) {
        std::fclose(file_);
        file_ = nullptr;
        throw PagerError("index file page size mismatch");
    }
    rootPage_ = loadU32(header.data() + kOffRoot);
    freeListHead_ = loadU32(header.data() + kOffFreeList);
    pageCount_ = loadU32(header.data() + kOffPageCount);
    wasClean_ = header[kOffClean] == 1;
    if (pageCount_ == 0) {
        pageCount_ = 1;
    }
}

Pager::~Pager() {
    if (file_ != nullptr) {
        try {
            flushAll();
        } catch (...) {
            // Destructor must not throw; a failed flush leaves cleanFlag=0 on
            // disk, which the next opener treats as RebuildRequired.
        }
        std::fclose(file_);
    }
}

void Pager::checkDataPageId(PageId id) const {
    if (id == 0 || id >= pageCount_) {
        throw PagerError("page id " + std::to_string(id) + " out of range (pageCount=" +
                         std::to_string(pageCount_) + ")");
    }
}

void Pager::writeHeaderToDisk(uint8_t cleanFlag) {
    std::vector<uint8_t> header(pageSize_, 0);
    std::memcpy(header.data(), kMagic, 4);
    storeU32(header.data() + kOffVersion, kVersion);
    storeU32(header.data() + kOffPageSize, pageSize_);
    storeU32(header.data() + kOffRoot, rootPage_);
    storeU32(header.data() + kOffFreeList, freeListHead_);
    storeU32(header.data() + kOffPageCount, pageCount_);
    header[kOffClean] = cleanFlag;
    std::fseek(file_, 0, SEEK_SET);
    if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
        throw PagerError("failed writing header page");
    }
    fsyncFile(file_);
}

void Pager::ensureUncleanOnDisk() {
    if (!dirtyMarked_) {
        writeHeaderToDisk(0);
        dirtyMarked_ = true;
    }
}

void Pager::filePut(PageId id, const uint8_t* buf) {
    std::fseek(file_, static_cast<long>(static_cast<uint64_t>(id) * pageSize_), SEEK_SET);
    if (std::fwrite(buf, 1, pageSize_, file_) != pageSize_) {
        throw PagerError("failed writing page " + std::to_string(id));
    }
}

void Pager::fileGet(PageId id, uint8_t* buf) {
    std::fseek(file_, static_cast<long>(static_cast<uint64_t>(id) * pageSize_), SEEK_SET);
    std::size_t got = std::fread(buf, 1, pageSize_, file_);
    if (got != pageSize_) {
        // Pages allocated but never flushed read back as zeroes.
        std::memset(buf + got, 0, pageSize_ - got);
    }
}

Pager::CacheEntry& Pager::cacheFetch(PageId id, bool loadFromFile) {
    auto it = cacheMap_.find(id);
    if (it != cacheMap_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return *lru_.begin();
    }
    evictIfNeeded();
    lru_.push_front(CacheEntry{id, std::vector<uint8_t>(pageSize_), false});
    cacheMap_[id] = lru_.begin();
    if (loadFromFile) {
        fileGet(id, lru_.begin()->data.data());
    }
    return *lru_.begin();
}

void Pager::evictIfNeeded() {
    while (lru_.size() >= cacheCapacity_ && !lru_.empty()) {
        CacheEntry& victim = lru_.back();
        if (victim.dirty) {
            filePut(victim.id, victim.data.data());
        }
        cacheMap_.erase(victim.id);
        lru_.pop_back();
    }
}

void Pager::readPage(PageId id, uint8_t* buf) {
    checkDataPageId(id);
    CacheEntry& e = cacheFetch(id, /*loadFromFile=*/true);
    std::memcpy(buf, e.data.data(), pageSize_);
}

void Pager::writePage(PageId id, const uint8_t* buf) {
    checkDataPageId(id);
    ensureUncleanOnDisk();
    // The page is fully overwritten, so skip the disk read on a cache miss.
    CacheEntry& e = cacheFetch(id, /*loadFromFile=*/false);
    std::memcpy(e.data.data(), buf, pageSize_);
    e.dirty = true;
}

PageId Pager::allocPage() {
    ensureUncleanOnDisk();
    if (freeListHead_ != 0) {
        PageId id = freeListHead_;
        std::vector<uint8_t> buf(pageSize_);
        readPage(id, buf.data());
        freeListHead_ = loadU32(buf.data());
        return id;
    }
    return pageCount_++;
}

void Pager::freePage(PageId id) {
    checkDataPageId(id);
    std::vector<uint8_t> buf(pageSize_, 0);
    storeU32(buf.data(), freeListHead_);
    writePage(id, buf.data());
    freeListHead_ = id;
}

void Pager::setRootPage(PageId id) {
    ensureUncleanOnDisk();
    rootPage_ = id;
}

void Pager::flushAll() {
    for (CacheEntry& e : lru_) {
        if (e.dirty) {
            filePut(e.id, e.data.data());
            e.dirty = false;
        }
    }
    fsyncFile(file_);
    writeHeaderToDisk(1);
    dirtyMarked_ = false;
}

} // namespace bisondb::btree

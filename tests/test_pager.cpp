#include "core/btree/pager.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace bisondb::btree;

namespace {

// Unique temp file per test, removed on destruction.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& name) {
        path = std::filesystem::temp_directory_path() / ("bisondb_test_" + name);
        std::filesystem::remove(path);
    }
    ~TempFile() { std::filesystem::remove(path); }
    std::string str() const { return path.string(); }
};

std::vector<uint8_t> filledPage(uint32_t pageSize, uint8_t fill) {
    return std::vector<uint8_t>(pageSize, fill);
}

} // namespace

TEST_CASE("pager creates, persists, and reopens", "[pager]") {
    TempFile f("pager_basic");
    {
        Pager p(f.str(), 256);
        REQUIRE(p.wasCleanOnOpen());
        REQUIRE(p.pageCount() == 1);
        PageId a = p.allocPage();
        PageId b = p.allocPage();
        REQUIRE(a == 1);
        REQUIRE(b == 2);
        p.writePage(a, filledPage(256, 0xAA).data());
        p.writePage(b, filledPage(256, 0xBB).data());
        p.setRootPage(a);
        p.flushAll();
    }
    {
        Pager p(f.str(), 256);
        REQUIRE(p.wasCleanOnOpen());
        REQUIRE(p.pageCount() == 3);
        REQUIRE(p.rootPage() == 1);
        std::vector<uint8_t> buf(256);
        p.readPage(1, buf.data());
        REQUIRE(buf[0] == 0xAA);
        p.readPage(2, buf.data());
        REQUIRE(buf[100] == 0xBB);
    }
}

TEST_CASE("pager rejects out-of-range page ids", "[pager]") {
    TempFile f("pager_range");
    Pager p(f.str(), 256);
    std::vector<uint8_t> buf(256);
    REQUIRE_THROWS_AS(p.readPage(0, buf.data()), PagerError); // header page is internal
    REQUIRE_THROWS_AS(p.readPage(1, buf.data()), PagerError); // beyond pageCount
    REQUIRE_THROWS_AS(p.writePage(5, buf.data()), PagerError);
    PageId a = p.allocPage();
    REQUIRE_NOTHROW(p.writePage(a, buf.data()));
}

TEST_CASE("freed pages are reused before extending the file", "[pager]") {
    TempFile f("pager_freelist");
    Pager p(f.str(), 256);
    PageId a = p.allocPage();
    PageId b = p.allocPage();
    p.writePage(a, filledPage(256, 1).data());
    p.writePage(b, filledPage(256, 2).data());
    p.freePage(a);
    REQUIRE(p.allocPage() == a); // from the free list
    REQUIRE(p.allocPage() == 3); // file grows again
}

TEST_CASE("free list survives flush and reopen", "[pager]") {
    TempFile f("pager_freelist_persist");
    {
        Pager p(f.str(), 256);
        PageId a = p.allocPage();
        PageId b = p.allocPage();
        p.writePage(a, filledPage(256, 1).data());
        p.writePage(b, filledPage(256, 2).data());
        p.freePage(b);
        p.freePage(a);
        p.flushAll();
    }
    Pager p(f.str(), 256);
    REQUIRE(p.allocPage() == 1); // LIFO free list
    REQUIRE(p.allocPage() == 2);
    REQUIRE(p.allocPage() == 3);
}

TEST_CASE("dirty flag: unclean close is visible to the next opener", "[pager]") {
    TempFile f("pager_dirty");
    {
        Pager p(f.str(), 256);
        p.flushAll();
    }
    {
        // Simulate a crash: mutate, then drop without flushAll by tearing the
        // header down by hand. The destructor flushes, so instead reopen the
        // raw file and verify the protocol mid-session.
        Pager p(f.str(), 256);
        PageId a = p.allocPage();
        p.writePage(a, filledPage(256, 7).data());
        // cleanFlag=0 must already be on disk before any data lands.
        Pager peek(f.str(), 256);
        REQUIRE_FALSE(peek.wasCleanOnOpen());
    }
    // The destructor's flush marked it clean again.
    Pager p(f.str(), 256);
    REQUIRE(p.wasCleanOnOpen());
}

TEST_CASE("pager rejects foreign files and size mismatches", "[pager]") {
    TempFile f("pager_magic");
    {
        std::ofstream out(f.path, std::ios::binary);
        out << "this is not an index file, definitely not 32 bytes of header";
    }
    REQUIRE_THROWS_AS(Pager(f.str(), 256), PagerError);

    TempFile g("pager_mismatch");
    {
        Pager p(g.str(), 256);
        p.flushAll();
    }
    REQUIRE_THROWS_AS(Pager(g.str(), 512), PagerError);
}

TEST_CASE("LRU cache evicts and rereads correctly", "[pager]") {
    TempFile f("pager_lru");
    Pager p(f.str(), 256, /*cacheCapacity=*/4);
    std::vector<PageId> ids;
    for (uint8_t i = 0; i < 20; ++i) {
        PageId id = p.allocPage();
        ids.push_back(id);
        p.writePage(id, filledPage(256, i).data());
    }
    std::vector<uint8_t> buf(256);
    for (uint8_t i = 0; i < 20; ++i) {
        p.readPage(ids[i], buf.data());
        REQUIRE(buf[42] == i);
    }
}

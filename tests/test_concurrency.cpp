// Single-writer / multi-reader exercise over one BTree. Run under TSan (the
// `tsan` preset) to verify the tree-level shared_mutex protocol; it also runs
// as a plain stress test on every platform.

#include "core/btree/btree.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace bisondb::btree;

namespace {

std::vector<uint8_t> numKey(uint32_t n) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08u", n);
    return std::vector<uint8_t>(buf, buf + 8);
}

} // namespace

TEST_CASE("concurrent readers with a single writer", "[btree][concurrency]") {
    // Unique per-process name: a crashed previous run may still hold the old
    // file open on Windows.
    std::random_device rd;
    auto path = std::filesystem::temp_directory_path() /
                ("bisondb_concurrency_" + std::to_string(rd()) + ".idx");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        BTreeOptions opts;
        opts.pageSize = 512;
        BTree tree(path.string(), opts);
        for (uint32_t i = 0; i < 500; ++i) {
            tree.insert(numKey(i), numKey(i));
        }

        std::atomic<bool> stop{false};
        std::atomic<std::size_t> reads{0};
        std::atomic<bool> readerFailed{false};

        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t) {
            readers.emplace_back([&tree, &stop, &reads, &readerFailed, t] {
                uint32_t at = static_cast<uint32_t>(t) * 100;
                while (!stop.load(std::memory_order_relaxed)) {
                    // Point reads on stable keys (the writer only touches
                    // keys >= 1000) and short scans.
                    auto got = tree.get(numKey(at % 500));
                    if (!got.has_value()) {
                        readerFailed = true;
                        return;
                    }
                    std::size_t steps = 0;
                    for (auto c = tree.lowerBound(numKey(at % 500)); c.valid() && steps < 10;
                         c.next()) {
                        ++steps;
                    }
                    at += 7;
                    reads.fetch_add(1, std::memory_order_relaxed);
                    // Give the writer room on rwlock implementations that
                    // would otherwise starve it under spinning readers.
                    std::this_thread::yield();
                }
            });
        }

        for (uint32_t i = 0; i < 2000; ++i) {
            tree.insert(numKey(1000 + i), numKey(i));
            if (i % 3 == 0) {
                tree.erase(numKey(1000 + i));
            }
        }
        stop = true;
        for (std::thread& t : readers) {
            t.join();
        }

        REQUIRE_FALSE(readerFailed.load());
        REQUIRE(reads.load() > 0);

        // Final state: original 500 keys plus the non-erased writer keys.
        std::size_t count = 0;
        for (auto c = tree.lowerBound({}); c.valid(); c.next()) {
            ++count;
        }
        std::size_t erased = (2000 + 2) / 3;
        REQUIRE(count == 500 + 2000 - erased);
    }
    std::filesystem::remove(path, ec);
}

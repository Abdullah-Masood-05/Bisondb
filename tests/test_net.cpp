#include "core/net/socket.hpp"
#include "core/net/thread_pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace bisondb::net;

namespace {

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

} // namespace

TEST_CASE("loopback echo", "[net]") {
    TcpListener listener("127.0.0.1", 0);
    REQUIRE(listener.port() != 0);

    std::thread serverThread([&listener] {
        TcpSocket conn = listener.accept();
        std::vector<uint8_t> buf(5);
        REQUIRE(conn.recvExact(buf) == RecvStatus::Complete);
        conn.sendAll(buf);
    });

    TcpSocket client = connectTcp("127.0.0.1", listener.port(), 2000);
    client.sendAll(bytes("hello"));
    std::vector<uint8_t> echo(5);
    REQUIRE(client.recvExact(echo) == RecvStatus::Complete);
    REQUIRE(echo == bytes("hello"));
    serverThread.join();
}

TEST_CASE("recvExact reassembles deliberately fragmented sends", "[net]") {
    TcpListener listener("127.0.0.1", 0);
    std::string message = "framing survives TCP boundaries";

    std::thread serverThread([&listener, &message] {
        TcpSocket conn = listener.accept();
        // One byte per send() call: the receiver must see message boundaries
        // imposed by its own framing, not by TCP segmentation.
        for (char c : message) {
            uint8_t b = static_cast<uint8_t>(c);
            conn.sendAll({&b, 1});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    TcpSocket client = connectTcp("127.0.0.1", listener.port(), 2000);
    std::vector<uint8_t> buf(message.size());
    REQUIRE(client.recvExact(buf) == RecvStatus::Complete);
    REQUIRE(std::string(buf.begin(), buf.end()) == message);
    serverThread.join();
}

TEST_CASE("recv timeout throws NetError(Timeout)", "[net]") {
    TcpListener listener("127.0.0.1", 0);
    std::thread serverThread([&listener] {
        TcpSocket conn = listener.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // never sends
    });

    TcpSocket client = connectTcp("127.0.0.1", listener.port(), 2000);
    client.setRecvTimeout(100);
    std::vector<uint8_t> buf(1);
    try {
        client.recvExact(buf);
        FAIL("expected NetError");
    } catch (const NetError& e) {
        REQUIRE(e.kind() == NetError::Kind::Timeout);
    }
    serverThread.join();
}

TEST_CASE("orderly close before any byte reports Closed, mid-message close throws", "[net]") {
    SECTION("clean close between messages") {
        TcpListener listener("127.0.0.1", 0);
        std::thread serverThread([&listener] {
            TcpSocket conn = listener.accept();
            conn.shutdownBoth();
        });
        TcpSocket client = connectTcp("127.0.0.1", listener.port(), 2000);
        std::vector<uint8_t> buf(4);
        REQUIRE(client.recvExact(buf) == RecvStatus::Closed);
        serverThread.join();
    }
    SECTION("close mid-message") {
        TcpListener listener("127.0.0.1", 0);
        std::thread serverThread([&listener] {
            TcpSocket conn = listener.accept();
            uint8_t two[2] = {1, 2};
            conn.sendAll(two);
            conn.shutdownBoth();
        });
        TcpSocket client = connectTcp("127.0.0.1", listener.port(), 2000);
        std::vector<uint8_t> buf(4); // expects 4, gets 2 then close
        try {
            client.recvExact(buf);
            FAIL("expected NetError");
        } catch (const NetError& e) {
            REQUIRE(e.kind() == NetError::Kind::Closed);
        }
        serverThread.join();
    }
}

TEST_CASE("connect to a dead port fails", "[net]") {
    uint16_t deadPort;
    {
        TcpListener probe("127.0.0.1", 0);
        deadPort = probe.port();
    } // closed: nothing listens there now
    REQUIRE_THROWS_AS(connectTcp("127.0.0.1", deadPort, 500), NetError);
}

TEST_CASE("listener close unblocks accept", "[net]") {
    TcpListener listener("127.0.0.1", 0);
    std::thread closer([&listener] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        listener.close();
    });
    REQUIRE_THROWS_AS(listener.accept(), NetError);
    closer.join();
}

TEST_CASE("thread pool runs tasks and survives task exceptions", "[net][threadpool]") {
    ThreadPool pool(4);
    REQUIRE(pool.threadCount() == 4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i) {
        pool.submit([&counter, i] {
            if (i % 10 == 0) {
                throw std::runtime_error("task failure must not kill the worker");
            }
            counter.fetch_add(1);
        });
    }
    pool.stop(); // drains
    REQUIRE(counter.load() == 90);
}

TEST_CASE("thread pool stop drains queued tasks", "[net][threadpool]") {
    std::atomic<int> ran{0};
    {
        ThreadPool pool(1);
        for (int i = 0; i < 50; ++i) {
            pool.submit([&ran] {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                ran.fetch_add(1);
            });
        }
    } // destructor stops and drains
    REQUIRE(ran.load() == 50);
}

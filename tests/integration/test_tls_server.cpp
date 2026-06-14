// Integration tests for TLS: a real bisond with --tls + auth, exercised through
// BisonClient over an encrypted socket. Covers the full secure stack, the
// verification matrix, plaintext<->TLS interop, and handshake-DoS resilience.
#include "client/client.hpp"
#include "core/crypto/crypto.hpp"
#include "core/net/socket.hpp"
#include "core/net/tls.hpp"
#include "server/server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <thread>

using namespace bisondb;
using bisondb::client::AuthError;
using bisondb::client::BisonClient;
using bisondb::client::Credentials;
using bisondb::client::TlsOptions;
namespace fs = std::filesystem;

namespace {

void setEnv(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}
void clearEnv(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

crypto::KdfParams fastKdf() {
    crypto::KdfParams p;
    p.memoryKiB = 64;
    p.passes = 1;
    p.lanes = 1;
    return p;
}

void writeFile(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary);
    f << content;
}

// A bisond with --tls (cert/key on disk) and an admin seeded from the env.
struct TlsFixture {
    fs::path dir;
    fs::path certPath;
    fs::path keyPath;
    std::string certFingerprint;
    std::unique_ptr<server::Server> srv;

    TlsFixture(const std::string& name, const net::CertKeyPem& ck, int handshakeTimeoutMs = 10000) {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("bisondb_tls_it_" + name + "_" + std::to_string(rd()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        certPath = dir / "cert.pem";
        keyPath = dir / "key.pem";
        writeFile(certPath, ck.certPem);
        writeFile(keyPath, ck.keyPem);
        certFingerprint = net::certFingerprintSha256(ck.certPem);

        setEnv("BISONDB_ADMIN_PASSWORD", "rootpw");
        server::ServerConfig config;
        config.dir = dir.string();
        config.port = 0;
        config.threads = 4;
        config.quiet = true;
        config.throttleAuth = false;
        config.kdf = fastKdf();
        config.initAdminUser = "root";
        config.tls = true;
        config.tlsCertFile = certPath.string();
        config.tlsKeyFile = keyPath.string();
        config.tlsHandshakeTimeoutMs = handshakeTimeoutMs;
        srv = std::make_unique<server::Server>(std::move(config));
        srv->start();
        clearEnv("BISONDB_ADMIN_PASSWORD");
    }
    ~TlsFixture() {
        if (srv) {
            srv->stop();
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    uint16_t port() const { return srv->port(); }
    TlsOptions pinned() const {
        TlsOptions t;
        t.enabled = true;
        t.verify = net::TlsVerify::Pin;
        t.pinSha256 = certFingerprint;
        t.hostname = "localhost";
        return t;
    }
    TlsOptions ca() const {
        TlsOptions t;
        t.enabled = true;
        t.verify = net::TlsVerify::CaFile;
        t.caFile = certPath.string();
        t.hostname = "localhost";
        return t;
    }
};

} // namespace

TEST_CASE("full secure stack: TLS handshake, login, CRUD", "[integration][tls]") {
    TlsFixture fx("stack", net::generateSelfSigned("localhost", 30));

    BisonClient c = BisonClient::connect("127.0.0.1", fx.port(), fx.pinned(),
                                         Credentials{"root", "rootpw", ""});
    REQUIRE(c.isTls());
    REQUIRE(c.tlsVerified());
    REQUIRE(c.authenticated());

    Value status = c.serverStatus();
    REQUIRE(status.asDocument().find("security")->asDocument().find("tls")->get<bool>() == true);

    REQUIRE(c.createCollection("t"));
    REQUIRE(c.insert("t", {Value(Document{{"x", Value(int32_t{1})}})}).size() == 1);
    REQUIRE(c.find("t", Value(Document{})).size() == 1);
}

TEST_CASE("wrong password over TLS still fails with AuthFailed", "[integration][tls]") {
    TlsFixture fx("badpw", net::generateSelfSigned("localhost", 30));
    try {
        BisonClient::connect("127.0.0.1", fx.port(), fx.pinned(), Credentials{"root", "WRONG", ""});
        FAIL("expected AuthFailed");
    } catch (const AuthError& e) {
        REQUIRE(e.code() == "AuthFailed");
    }
}

TEST_CASE("verification matrix", "[integration][tls]") {
    TlsFixture fx("verify", net::generateSelfSigned("localhost", 30));
    Credentials root{"root", "rootpw", ""};

    SECTION("CaFile with the server cert as trust anchor succeeds") {
        BisonClient c = BisonClient::connect("127.0.0.1", fx.port(), fx.ca(), root);
        REQUIRE(c.authenticated());
        REQUIRE(c.tlsVerified());
    }
    SECTION("correct pin succeeds, wrong pin fails") {
        REQUIRE_NOTHROW(BisonClient::connect("127.0.0.1", fx.port(), fx.pinned(), root));
        TlsOptions bad = fx.pinned();
        bad.pinSha256 = std::string(64, 'b');
        REQUIRE_THROWS_AS(BisonClient::connect("127.0.0.1", fx.port(), bad, root), net::TlsError);
    }
    SECTION("hostname mismatch fails") {
        TlsOptions t = fx.ca();
        t.hostname = "wronghost"; // cert CN is localhost
        REQUIRE_THROWS_AS(BisonClient::connect("127.0.0.1", fx.port(), t, root), net::TlsError);
    }
    SECTION("unknown CA fails") {
        TlsOptions t = fx.ca();
        // Trust a DIFFERENT self-signed cert; the server's cert won't chain to it.
        fs::path other = fx.dir / "other.pem";
        writeFile(other, net::generateSelfSigned("localhost", 30).certPem);
        t.caFile = other.string();
        REQUIRE_THROWS_AS(BisonClient::connect("127.0.0.1", fx.port(), t, root), net::TlsError);
    }
    SECTION("--tls-insecure connects despite an untrusted cert") {
        TlsOptions t;
        t.enabled = true;
        t.verify = net::TlsVerify::Insecure;
        BisonClient c = BisonClient::connect("127.0.0.1", fx.port(), t, root);
        REQUIRE(c.isTls());
        REQUIRE_FALSE(c.tlsVerified()); // encrypted but UNVERIFIED
        REQUIRE(c.authenticated());
    }
}

TEST_CASE("expired certificate is rejected", "[integration][tls]") {
    TlsFixture fx("expired", net::generateSelfSigned("localhost", -1)); // expired yesterday
    TlsOptions t = fx.ca();
    REQUIRE_THROWS_AS(
        BisonClient::connect("127.0.0.1", fx.port(), t, Credentials{"root", "rootpw", ""}),
        net::TlsError);
}

TEST_CASE("plaintext<->TLS mismatch fails fast, server survives", "[integration][tls]") {
    TlsFixture fx("interop", net::generateSelfSigned("localhost", 30));
    Credentials root{"root", "rootpw", ""};

    // Plaintext client against a TLS server: the auth handshake can't complete.
    REQUIRE_THROWS(BisonClient::connect("127.0.0.1", fx.port(), root));

    // TLS client against... well, here the server IS TLS, so a healthy TLS
    // client still works afterwards (server survived the bad connection).
    REQUIRE_NOTHROW(BisonClient::connect("127.0.0.1", fx.port(), fx.pinned(), root));
}

TEST_CASE("TLS client against a plaintext server fails with a TLS handshake error",
          "[integration][tls]") {
    // A plaintext (no-TLS) auth server.
    std::random_device rd;
    fs::path dir = fs::temp_directory_path() / ("bisondb_plain_" + std::to_string(rd()));
    fs::remove_all(dir);
    setEnv("BISONDB_ADMIN_PASSWORD", "rootpw");
    server::ServerConfig config;
    config.dir = dir.string();
    config.port = 0;
    config.quiet = true;
    config.throttleAuth = false;
    config.kdf = fastKdf();
    config.initAdminUser = "root";
    auto srv = std::make_unique<server::Server>(std::move(config));
    srv->start();
    clearEnv("BISONDB_ADMIN_PASSWORD");

    TlsOptions t;
    t.enabled = true;
    t.verify = net::TlsVerify::Insecure;
    try {
        BisonClient::connect("127.0.0.1", srv->port(), t, Credentials{"root", "rootpw", ""});
        FAIL("expected a TLS handshake failure against a plaintext server");
    } catch (const net::TlsError& e) {
        REQUIRE(e.kind() == net::TlsError::Kind::Handshake);
    }
    srv->stop();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("a stalled TLS handshake does not block other clients", "[integration][tls]") {
    TlsFixture fx("dos", net::generateSelfSigned("localhost", 30), /*handshakeTimeoutMs=*/800);

    // Open a raw TCP connection and send NOTHING — the worker's handshake will
    // block, then time out, without ever stalling the acceptor.
    net::TcpSocket stalled = net::connectTcp("127.0.0.1", fx.port(), 2000);

    // A healthy TLS client connects and works while the stalled one hangs.
    auto begin = std::chrono::steady_clock::now();
    BisonClient healthy = BisonClient::connect("127.0.0.1", fx.port(), fx.pinned(),
                                               Credentials{"root", "rootpw", ""});
    REQUIRE_NOTHROW(healthy.ping());
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - begin)
                         .count();
    REQUIRE(elapsedMs < 2000); // not blocked behind the stalled handshake

    stalled.close();
}

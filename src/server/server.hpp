#pragma once

#include "core/auth/token_store.hpp"
#include "core/auth/user_store.hpp"
#include "core/crypto/crypto.hpp"
#include "core/net/socket.hpp"
#include "core/net/thread_pool.hpp"
#include "core/query/database.hpp"
#include "server/auth_session.hpp"
#include "server/protocol.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace bisondb::server {

struct ServerConfig {
    std::string dir;
    std::string bind = "127.0.0.1";
    uint16_t port = 27027;   // 0 = ephemeral, read back via Server::port()
    std::size_t threads = 0; // 0 = hardware_concurrency
    std::size_t maxConnections = 64;
    std::size_t maxMessageSize = kMaxMessageSize; // shrinkable for tests
    bool quiet = false;

    // ── Authentication ────────────────────────────────────────────────────
    // Auth is ON unless explicitly disabled. WARNING: the transport is NOT
    // encrypted yet (no TLS) — credentials travel in clear text. Trusted
    // networks only until the TLS phase ships.
    bool noAuth = false;       // --no-auth dev escape hatch (loud, loopback-only)
    std::string initAdminUser; // --init-admin <user>; password from BISONDB_ADMIN_PASSWORD
    std::int64_t tokenTtlSeconds = 3600; // session token lifetime
    bool throttleAuth = true;            // real sleep on failed-auth backoff (tests disable)
    crypto::KdfParams kdf{};             // Argon2id cost; tests lower it for speed
};

struct ServerStats {
    std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
    std::atomic<std::size_t> connectionsCurrent{0};
    std::mutex opMutex;
    std::map<std::string, uint64_t> opCounters;

    void countOp(const std::string& cmd) {
        std::lock_guard lock(opMutex);
        ++opCounters[cmd];
    }
};

// The bisond daemon as a library (main.cpp is a thin flag-parsing wrapper,
// so integration tests run the server in-process on an ephemeral port).
class Server {
  public:
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds the listener and starts the acceptor thread.
    void start();

    // Bound port (after start(); resolves ephemeral port 0).
    uint16_t port() const noexcept { return port_; }

    // Signal-handler-safe stop trigger: stops accepting and wakes the
    // shutdown path. The full drain happens in stop()/waitUntilStopped().
    void requestStop() noexcept;

    // Blocks until requestStop(), then drains and syncs (used by main()).
    void waitUntilStopped();

    // Full graceful shutdown: stop accepting, close idle connections after a
    // grace period, drain workers, sync all collections. Idempotent.
    void stop();

    query::Database& database() noexcept { return db_; }
    const ServerConfig& config() const noexcept { return config_; }
    ServerStats& stats() noexcept { return stats_; }

    // ── Auth surface used by the command dispatcher ───────────────────────
    // True when auth enforcement is in effect (i.e. not --no-auth).
    bool authActive() const noexcept { return !config_.noAuth; }
    // True until the first admin is created (no users on disk, no --init-admin).
    bool inSetupMode() const noexcept { return setupMode_.load(); }
    auth::UserStore& users() noexcept { return *users_; }
    auth::TokenStore& tokens() noexcept { return tokens_; }

    // Constant-time check of a presented bootstrap token against the one-time
    // value printed at startup. False once setup mode has ended.
    bool checkBootstrapToken(const std::string& presented) const;
    // Ends setup mode and wipes the bootstrap token (after the first admin).
    void endSetupMode();

    // The live one-time bootstrap token (empty once setup mode ends). This is
    // the same value printed to STDERR at startup; exposed so in-process tests
    // can drive the bootstrap flow.
    std::string bootstrapToken() const;

    // Audit line for auth events (never carries secrets); routes through log().
    void auditLog(const std::string& line) { log(line); }

  private:
    void acceptLoop();
    void serveConnection(net::TcpSocket socket, uint64_t connId);
    void log(const std::string& line);
    void initAuth(); // construct user store + run first-run bootstrap

    ServerConfig config_;
    query::Database db_;
    ServerStats stats_;
    std::unique_ptr<net::TcpListener> listener_;
    std::unique_ptr<net::ThreadPool> pool_;
    std::thread acceptor_;
    uint16_t port_ = 0;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> stopped_{false};
    std::mutex stopMutex_;
    std::condition_variable stopCv_;

    // Live connections, for the forced-close pass during shutdown.
    std::mutex connMutex_;
    std::unordered_map<uint64_t, net::TcpSocket*> connections_;
    std::atomic<uint64_t> nextConnId_{1};

    // Auth state. users_ is null only in --no-auth mode.
    std::unique_ptr<auth::UserStore> users_;
    auth::TokenStore tokens_;
    std::atomic<bool> setupMode_{false};
    mutable std::mutex bootstrapMutex_;
    std::string bootstrapToken_; // one-time secret, cleared when setup ends
};

// Command dispatch (commands.cpp). Returns the response document; never
// throws — every engine exception is translated to an { ok:false, error }
// response in one place. `conn` carries the per-connection auth state and is
// mutated by the auth handshake commands.
Value dispatchCommand(Server& server, const Value& request, const net::TcpSocket& peer,
                      ConnectionAuth& conn);

} // namespace bisondb::server

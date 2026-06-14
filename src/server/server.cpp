#include "server/server.hpp"

#include "core/json_writer.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace bisondb::server {

namespace {

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return std::to_string(ms.count());
}

} // namespace

Server::Server(ServerConfig config) : config_(std::move(config)), db_(config_.dir) {
    initAuth();
    initTls();
}

Server::~Server() {
    stop();
}

void Server::initAuth() {
    if (!authActive()) {
        // --no-auth: every client gets full access. Shout about it on every
        // startup; the non-loopback bind refusal lives in start().
        log("WARNING ============================================================");
        log("WARNING  --no-auth: AUTHENTICATION IS DISABLED. Every client has");
        log("WARNING  full access to all data and admin commands. Use only on a");
        log("WARNING  trusted, loopback-only host. Never on a shared network.");
        log("WARNING ============================================================");
        return;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(config_.dir, ec);
    users_ = std::make_unique<auth::UserStore>(config_.dir, config_.kdf);

    if (!users_->empty()) {
        return; // normal mode: users exist, clients must authenticate
    }

    // First run, no users. Either seed an admin from the environment, or enter
    // setup mode and print a one-time bootstrap token.
    if (!config_.initAdminUser.empty()) {
        const char* pw = std::getenv("BISONDB_ADMIN_PASSWORD");
        if (pw == nullptr || pw[0] == '\0') {
            throw std::runtime_error("--init-admin set but BISONDB_ADMIN_PASSWORD is empty/unset");
        }
        users_->createUser(config_.initAdminUser, pw, {auth::Role::Admin});
        log("info  admin user created: " + config_.initAdminUser);
        return;
    }

    crypto::Token bt = crypto::generateToken();
    {
        std::lock_guard lock(bootstrapMutex_);
        bootstrapToken_ = bt.raw;
    }
    setupMode_.store(true);
    // Printed straight to stderr (even under --quiet) so an operator can copy
    // it, but NEVER written to the structured log stream.
    std::cerr << "\n==================== BisonDB SETUP MODE ====================\n"
              << "No users exist yet. Create the first admin using this one-time\n"
              << "bootstrap token (valid only until the first admin is created):\n\n"
              << "    " << bt.raw << "\n\n"
              << "  bisonsh           then:  auth bootstrap <username>\n"
              << "  or offline:  bisonc auth create-admin --dir <dir> --username <u>\n\n"
              << "WARNING: the connection is NOT encrypted (no TLS yet); credentials\n"
              << "travel in clear text. Use only on trusted networks.\n"
              << "============================================================\n\n";
}

void Server::initTls() {
    if (!config_.tls) {
        return;
    }
    if (config_.tlsSelfSigned && config_.tlsCertFile.empty()) {
        net::CertKeyPem ck = net::generateSelfSigned("localhost", 365);
        tlsContext_ = net::TlsContext::serverFromPem(ck.certPem, ck.keyPem);
        // Printed to stderr (even under --quiet) so a client can pin it; the
        // private key itself is NEVER logged.
        std::cerr << "\n=============== BisonDB TLS (self-signed) ===============\n"
                  << "Generated an in-memory self-signed certificate (CN=localhost).\n"
                  << "It is NOT trusted by any CA — pin it on the client with:\n\n"
                  << "    --tls-pin " << net::certFingerprintSha256(ck.certPem) << "\n\n"
                  << "For anything beyond local dev, use a real cert via --tls-cert/--tls-key\n"
                  << "(see: bisonc tls gen-cert).\n"
                  << "=========================================================\n\n";
    } else {
        if (config_.tlsCertFile.empty() || config_.tlsKeyFile.empty()) {
            throw std::runtime_error(
                "--tls requires --tls-cert and --tls-key (or --tls-self-signed)");
        }
        tlsContext_ = net::TlsContext::serverFromFiles(config_.tlsCertFile, config_.tlsKeyFile);
    }
}

bool Server::checkBootstrapToken(const std::string& presented) const {
    std::lock_guard lock(bootstrapMutex_);
    if (!setupMode_.load() || bootstrapToken_.empty()) {
        return false;
    }
    return crypto::constantTimeEquals(presented, bootstrapToken_);
}

void Server::endSetupMode() {
    std::lock_guard lock(bootstrapMutex_);
    setupMode_.store(false);
    bootstrapToken_.clear();
}

std::string Server::bootstrapToken() const {
    std::lock_guard lock(bootstrapMutex_);
    return bootstrapToken_;
}

void Server::log(const std::string& line) {
    if (!config_.quiet) {
        std::cerr << "[" << timestamp() << "] " << line << "\n";
    }
}

void Server::start() {
    if (!authActive() && config_.bind != "127.0.0.1" && config_.bind != "::1" &&
        config_.bind != "localhost") {
        throw std::runtime_error("--no-auth refuses to bind to a non-loopback address: " +
                                 config_.bind);
    }
    listener_ = std::make_unique<net::TcpListener>(config_.bind, config_.port);
    port_ = listener_->port();
    std::size_t threads =
        config_.threads != 0 ? config_.threads : std::thread::hardware_concurrency();
    pool_ = std::make_unique<net::ThreadPool>(threads);
    acceptor_ = std::thread([this] { acceptLoop(); });
    log("info  bisond listening on " + config_.bind + ":" + std::to_string(port_) +
        " dir=" + config_.dir + " threads=" + std::to_string(threads));
}

void Server::acceptLoop() {
    while (!stopRequested_.load()) {
        net::TcpSocket sock;
        try {
            sock = listener_->accept();
        } catch (const net::NetError&) {
            break; // listener closed (shutdown) or fatal accept error
        }
        if (stopRequested_.load()) {
            break;
        }
        if (stats_.connectionsCurrent.load() >= config_.maxConnections) {
            // Best-effort rejection. Skip it for TLS: the peer is still mid-
            // handshake and couldn't read a plaintext frame anyway.
            if (!tlsEnabled()) {
                try {
                    net::TcpStream rejected(std::move(sock));
                    Value busy(Document{
                        {"ok", Value(false)},
                        {"error",
                         Value(Document{{"code", Value("ServerBusy")},
                                        {"message", Value("connection limit reached")}})}});
                    writeFrame(rejected, busy, config_.maxMessageSize);
                } catch (...) {
                }
            }
            continue; // socket (or the temp stream) closes via RAII
        }
        uint64_t connId = nextConnId_.fetch_add(1);
        stats_.connectionsCurrent.fetch_add(1);
        // The pool owns the connection for its whole lifetime.
        auto shared = std::make_shared<net::TcpSocket>(std::move(sock));
        pool_->submit(
            [this, shared, connId]() mutable { serveConnection(std::move(*shared), connId); });
    }
}

void Server::serveConnection(net::TcpSocket socket, uint64_t connId) {
    // Build the byte stream. For TLS, run the handshake HERE (in the worker
    // thread, not the acceptor) so a slow/malicious handshake can't stall
    // accepting; a recv timeout bounds it.
    std::unique_ptr<net::Stream> stream;
    if (tlsEnabled()) {
        socket.setRecvTimeout(config_.tlsHandshakeTimeoutMs);
        try {
            stream = std::make_unique<net::TlsStream>(
                net::TlsStream::accept(tlsContext_, std::move(socket)));
        } catch (const net::TlsError& e) {
            log("debug conn=" + std::to_string(connId) + " TLS handshake failed: " + e.what());
            stats_.connectionsCurrent.fetch_sub(1);
            return; // socket closed via RAII
        }
        stream->setRecvTimeout(0); // blocking again for normal request handling
    } else {
        stream = std::make_unique<net::TcpStream>(std::move(socket));
    }

    {
        std::lock_guard lock(connMutex_);
        connections_[connId] = stream.get();
    }
    log("info  conn=" + std::to_string(connId) + (tlsEnabled() ? " accepted (TLS)" : " accepted"));

    // Per-connection auth state; lives for the whole connection lifetime.
    ConnectionAuth conn;

    // Strictly sequential: read one request, write one response, repeat.
    while (!stopRequested_.load()) {
        std::optional<Value> request;
        try {
            request = readFrame(*stream, config_.maxMessageSize);
        } catch (const FrameError& e) {
            // Byte stream out of sync: best-effort error frame, then close.
            try {
                Value err(Document{
                    {"ok", Value(false)},
                    {"error", Value(Document{{"code", Value("TooLarge")},
                                             {"message", Value(std::string(e.what()))}})}});
                writeFrame(*stream, err, config_.maxMessageSize);
            } catch (...) {
            }
            break;
        } catch (const BsonParseError& e) {
            // Framed but malformed payload: report and keep serving.
            try {
                Value err(Document{
                    {"ok", Value(false)},
                    {"error", Value(Document{{"code", Value("ParseError")},
                                             {"message", Value(std::string(e.what()))}})}});
                writeFrame(*stream, err, config_.maxMessageSize);
                continue;
            } catch (...) {
                break;
            }
        } catch (const net::NetError&) {
            break; // timeout / reset / closed mid-frame
        }
        if (!request.has_value()) {
            break; // clean disconnect between requests
        }

        auto begun = std::chrono::steady_clock::now();
        Value response = dispatchCommand(*this, *request, *stream, conn);
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - begun)
                              .count();
        std::string cmd = "?";
        if (request->is<Document>()) {
            if (const Value* c = request->asDocument().find("cmd"); c && c->is<std::string>()) {
                cmd = c->get<std::string>();
            }
        }
        log("info  conn=" + std::to_string(connId) + " cmd=" + cmd +
            " durationMs=" + std::to_string(durationMs));
        try {
            writeFrame(*stream, response, config_.maxMessageSize);
        } catch (...) {
            break;
        }
    }

    {
        std::lock_guard lock(connMutex_);
        connections_.erase(connId);
    }
    stats_.connectionsCurrent.fetch_sub(1);
    log("info  conn=" + std::to_string(connId) + " closed");
}

void Server::requestStop() noexcept {
    bool expected = false;
    if (!stopRequested_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (listener_) {
        listener_->close(); // unblocks accept()
    }
    stopCv_.notify_all();
}

void Server::waitUntilStopped() {
    std::unique_lock lock(stopMutex_);
    stopCv_.wait(lock, [this] { return stopRequested_.load(); });
    lock.unlock();
    stop();
}

void Server::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    requestStop();
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    // Grace period for in-flight responses, then shut idle readers down so
    // their blocking recv calls return.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard lock(connMutex_);
        for (auto& [id, sock] : connections_) {
            sock->shutdownBoth();
        }
    }
    if (pool_) {
        pool_->stop();
    }
    db_.syncAll();
    log("info  bisond stopped");
}

} // namespace bisondb::server

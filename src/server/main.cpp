#include "core/platform.hpp"
#include "core/version.hpp"
#include "server/server.hpp"
#include "shell/banner.hpp"
#include "shell/printer.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#if defined(BISONDB_PLATFORM_WINDOWS)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace {

bisondb::server::Server* g_server = nullptr;

#if defined(BISONDB_PLATFORM_WINDOWS)
BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_server != nullptr) {
            g_server->requestStop();
        }
        return TRUE;
    }
    return FALSE;
}
void installSignalHandlers() {
    SetConsoleCtrlHandler(consoleHandler, TRUE);
}
#else
extern "C" void signalHandler(int) {
    if (g_server != nullptr) {
        g_server->requestStop();
    }
}
void installSignalHandlers() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}
#endif

int usage() {
    std::cerr << "bisond " << bisondb::version() << " - BisonDB server\n\n"
              << "Usage: bisond --dir <dbdir> [--port N] [--bind ADDR] [--threads N] [--quiet]\n"
              << "              [--init-admin <user>] [--no-auth] [--token-ttl <sec>]\n\n"
              << "  --dir         database directory (required)\n"
              << "  --port        TCP port (default 27027)\n"
              << "  --bind        bind address (default 127.0.0.1)\n"
              << "  --threads     worker threads (default: hardware concurrency)\n"
              << "  --quiet       suppress per-request logging\n"
              << "  --init-admin  on first run, create this admin from $BISONDB_ADMIN_PASSWORD\n"
              << "  --no-auth     DANGER: disable authentication entirely (loopback only)\n"
              << "  --token-ttl   session-token lifetime in seconds (default 3600)\n"
              << "  --tls         encrypt the transport (TLS 1.2); needs a cert/key\n"
              << "  --tls-cert    PEM certificate (chain) file\n"
              << "  --tls-key     PEM private key file\n"
              << "  --tls-self-signed  generate an in-memory self-signed cert at startup\n"
              << "                     and print its SHA-256 fingerprint for client pinning\n\n"
              << "Authentication is ENABLED by default. On first run with no users, bisond\n"
              << "prints a one-time bootstrap token to create the first admin.\n\n"
              << "Use `bisonc tls gen-cert` to make a cert/key pair, then run with --tls.\n"
              << "Without --tls the transport is UNENCRYPTED: credentials and data travel in\n"
              << "clear text. Use --tls (or a trusted network) — especially off loopback.\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    bisondb::server::ServerConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "bisond: " << arg << " requires a value\n";
                std::exit(usage());
            }
            return argv[++i];
        };
        if (arg == "--dir") {
            config.dir = next();
        } else if (arg == "--port") {
            config.port = static_cast<uint16_t>(std::stoi(next()));
        } else if (arg == "--bind") {
            config.bind = next();
        } else if (arg == "--threads") {
            config.threads = static_cast<std::size_t>(std::stoul(next()));
        } else if (arg == "--quiet") {
            config.quiet = true;
        } else if (arg == "--init-admin") {
            config.initAdminUser = next();
        } else if (arg == "--no-auth") {
            config.noAuth = true;
        } else if (arg == "--token-ttl") {
            config.tokenTtlSeconds = static_cast<std::int64_t>(std::stoll(next()));
        } else if (arg == "--tls") {
            config.tls = true;
        } else if (arg == "--tls-cert") {
            config.tlsCertFile = next();
        } else if (arg == "--tls-key") {
            config.tlsKeyFile = next();
        } else if (arg == "--tls-self-signed") {
            config.tls = true;
            config.tlsSelfSigned = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "bisond: unknown flag " << arg << "\n";
            return usage();
        }
    }
    if (config.dir.empty()) {
        std::cerr << "bisond: --dir is required\n";
        return usage();
    }
    if (config.bind != "127.0.0.1" && !config.tls && !config.quiet) {
        std::cerr << "WARNING: binding to " << config.bind
                  << " WITHOUT --tls — credentials and data are exposed in clear text to\n"
                     "the network. Add --tls (see `bisonc tls gen-cert`).\n";
    }

    if (!config.quiet) {
        using namespace bisondb::shell;
        bool utf8Ok = enableUtf8Output();
        ColorProbe probe = systemProbe(/*noColorFlag=*/false);
        std::cerr << renderBanner(detectColorMode(probe), bisondb::version(), "server",
                                  wantAsciiBanner(probe, utf8Ok));
    }

    try {
        bisondb::server::Server server(std::move(config));
        g_server = &server;
        installSignalHandlers();
        server.start();
        server.waitUntilStopped();
        g_server = nullptr;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bisond: " << e.what() << "\n";
        return 2;
    }
}

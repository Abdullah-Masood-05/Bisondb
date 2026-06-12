#include "core/version.hpp"
#include "shell/printer.hpp"
#include "shell/repl.hpp"

#include <fstream>
#include <iostream>
#include <string>

namespace {

int usage() {
    std::cerr << "bisonsh " << bisondb::version() << " - BisonDB interactive shell\n\n"
              << "Usage: bisonsh [--connect host:port] [--no-color]\n"
              << "               [--eval '<stmt>[; <stmt>...]'] [-f script.bsh]\n\n"
              << "Default server: 127.0.0.1:27027. With --eval, -f, or piped stdin the\n"
              << "shell runs non-interactively and exits non-zero on the first error.\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    using namespace bisondb::shell;
    ShellConfig config;
    std::string evalText;
    std::string scriptPath;
    bool noColor = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "bisonsh: " << arg << " requires a value\n";
                std::exit(usage());
            }
            return argv[++i];
        };
        if (arg == "--connect") {
            std::string endpoint = next();
            std::size_t colon = endpoint.rfind(':');
            if (colon == std::string::npos) {
                std::cerr << "bisonsh: --connect expects host:port\n";
                return usage();
            }
            config.host = endpoint.substr(0, colon);
            config.port = static_cast<uint16_t>(std::stoi(endpoint.substr(colon + 1)));
        } else if (arg == "--no-color") {
            noColor = true;
        } else if (arg == "--eval") {
            evalText = next();
        } else if (arg == "-f") {
            scriptPath = next();
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "bisonsh: unknown flag " << arg << "\n";
            return usage();
        }
    }

    bool scripted = !evalText.empty() || !scriptPath.empty() || !stdinIsTty();
    config.interactive = !scripted;
    config.color = !noColor && !scripted && stdoutIsTty();
    if (config.color) {
        enableVtProcessing();
    }
    if (config.interactive) {
        config.historyPath = homeDirectory() + "/.bisonsh_history";
    }

    try {
        Shell shell(config, std::cin, std::cout, std::cerr);
        if (!evalText.empty()) {
            return shell.evalString(evalText);
        }
        if (!scriptPath.empty()) {
            std::ifstream script(scriptPath);
            if (!script) {
                std::cerr << "bisonsh: cannot open " << scriptPath << "\n";
                return 2;
            }
            return shell.runScript(script);
        }
        if (!config.interactive) {
            return shell.runScript(std::cin);
        }
        return shell.runInteractive();
    } catch (const std::exception& e) {
        std::cerr << "bisonsh: " << e.what() << "\n";
        return 2;
    }
}

#include "core/secure_input.hpp"

#include "core/platform.hpp"

#include <iostream>

#if defined(BISONDB_PLATFORM_WINDOWS)
    #include <conio.h>
    #include <io.h>
    #define BISONDB_ISATTY _isatty
    #define BISONDB_FILENO _fileno
#else
    #include <termios.h>
    #include <unistd.h>
#endif

namespace bisondb {

bool stdinIsTty() {
#if defined(BISONDB_PLATFORM_WINDOWS)
    return BISONDB_ISATTY(BISONDB_FILENO(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

std::string readPasswordFromTty(const std::string& prompt) {
    std::cerr << prompt;
    std::cerr.flush();

    if (!stdinIsTty()) {
        // Piped input: just read a line normally (no echo to suppress).
        std::string line;
        std::getline(std::cin, line);
        std::cerr << "\n";
        return line;
    }

#if defined(BISONDB_PLATFORM_WINDOWS)
    std::string out;
    for (;;) {
        int ch = _getch();
        if (ch == '\r' || ch == '\n' || ch == EOF) {
            break;
        }
        if (ch == '\b') { // backspace
            if (!out.empty()) {
                out.pop_back();
            }
            continue;
        }
        if (ch == 3) { // Ctrl-C
            out.clear();
            break;
        }
        out.push_back(static_cast<char>(ch));
    }
    std::cerr << "\n";
    return out;
#else
    termios oldt{};
    tcgetattr(STDIN_FILENO, &oldt);
    termios newt = oldt;
    newt.c_lflag &= static_cast<tcflag_t>(~ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    std::string line;
    std::getline(std::cin, line);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::cerr << "\n";
    return line;
#endif
}

} // namespace bisondb

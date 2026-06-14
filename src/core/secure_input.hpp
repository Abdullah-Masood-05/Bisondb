#pragma once

#include <string>

namespace bisondb {

// Reads a line from the terminal WITHOUT echoing it — for passwords. The prompt
// is written to stderr. If stdin is not a TTY (piped/redirected), it falls back
// to a normal line read so scripts/tests still work. The trailing newline is
// stripped.
std::string readPasswordFromTty(const std::string& prompt);

// True if standard input is an interactive terminal.
bool stdinIsTty();

} // namespace bisondb

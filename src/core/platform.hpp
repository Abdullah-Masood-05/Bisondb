#pragma once

// Platform detection macros.
// All socket/networking code must go through the future net:: wrapper namespace.
// Never use raw Winsock or POSIX socket APIs directly in application code.

#if defined(_WIN32)
    #define BISONDB_PLATFORM_WINDOWS 1
#elif defined(__linux__)
    #define BISONDB_PLATFORM_LINUX 1
#elif defined(__APPLE__)
    #define BISONDB_PLATFORM_MACOS 1
#else
    #error "Unsupported platform"
#endif

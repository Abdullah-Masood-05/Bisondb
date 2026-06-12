# BisonDB

BisonDB is a document database with a BSON storage engine, inspired by MongoDB. It is written
in C++20 and targets Windows as its primary development platform, with Linux support planned.
The project is currently in its foundation phase (Phase 0): the build system, CI pipeline, and
code quality tooling are in place, but no storage or networking code exists yet.

## Building

### Prerequisites

- CMake 3.21+
- Ninja (recommended) or Visual Studio 2022
- MSVC, GCC, or Clang with C++20 support

### Quick start (MSVC / Visual Studio)

```bat
cmake --preset msvc
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

### Quick start (Ninja)

```bat
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

### Run the CLI

```bat
.\build\debug\bisondb.exe
```

Expected output: `BisonDB 0.1.0`

## Code formatting

This project uses `clang-format` with the configuration in `.clang-format` (LLVM style, 4-space
indent, 100-column limit, left pointer alignment). To check formatting locally:

```bash
clang-format --dry-run --Werror $(find src tests -name "*.cpp" -o -name "*.hpp")
```

CI will fail the lint job if any file is not formatted correctly.

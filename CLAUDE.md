# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Ladybird is a truly independent web browser with a novel engine based on web standards. It uses a multi-process architecture with separate processes for UI, WebContent rendering, ImageDecoder, and RequestServer. The project is in pre-alpha state and is built with C++23.

## Fork Information

**This is a personal fork with experimental IPC security enhancements for learning and research purposes.**

The fork includes:
- **LibIPC Security Utilities**: RateLimiter, ValidatedDecoder, SafeMath, Limits
- **IPC Fuzzing Framework**: Automated security testing for IPC messages
- **Enhanced Service Hardening**: Additional validation in WebContent, ImageDecoder, and RequestServer
- **Fork Documentation**: See `claudedocs/` for detailed implementation guides and Windows build instructions

**Important**: These experimental features are for educational purposes only and are not intended for upstream contribution.

For upstream Ladybird documentation, see the official repository at https://github.com/LadybirdBrowser/ladybird

## Build System & Commands

### Building and Running

The primary build script is `./Meta/ladybird.py`:

```bash
# Build and run Ladybird (Release build)
./Meta/ladybird.py run

# Build and run in Debug mode
BUILD_PRESET=Debug ./Meta/ladybird.py run

# Run with gdb
./Meta/ladybird.py gdb ladybird

# Run other executables (JS REPL, WebAssembly REPL, etc.)
./Meta/ladybird.py run <executable_name>
```

### CMake Presets

The project uses CMake presets defined in `CMakePresets.json`:

- **Release**: Default release build (`Build/release`)
- **Debug**: Debug build (`Build/debug`)
- **Sanitizer**: Build with ASAN and UBSAN (`Build/sanitizers`)
- **Distribution**: Static library build for distribution
- **Fuzzers**: Fuzzer build with libFuzzer

```bash
# Using CMake directly
cmake --preset Release
cmake --build --preset Release
ctest --preset Release
```

### Testing

```bash
# Run all tests
./Meta/ladybird.py test

# Run LibWeb tests only
./Meta/ladybird.py test LibWeb

# Run test-web directly
./Meta/ladybird.py run test-web

# Using ninja directly
cd Build/release
ninja test

# Show output on failure
CTEST_OUTPUT_ON_FAILURE=1 ninja test

# Rebaseline Text/Layout tests
./Meta/ladybird.py run test-web --rebaseline -f Text/input/your-test-name.html

# Run Web Platform Tests
./Meta/WPT.sh run --log results.log
./Meta/WPT.sh compare --log results.log expectations.log css

# Import WPT tests
./Meta/WPT.sh import html/dom/aria-attribute-reflection.html

# Run IPC security tests (fork-specific)
./Meta/ladybird.py test LibIPC

# Run IPC fuzzing tests (if built with Fuzzers preset)
./Build/fuzzers/bin/FuzzIPC
./Build/fuzzers/bin/FuzzWebContentIPC
```

### Linting and Code Style

```bash
# Run clang-format (automatic formatting)
ninja -C Build/release check-style

# Run shell script linting
ninja -C Build/release lint-shell-scripts

# Pre-commit hooks (optional but recommended)
pre-commit install                    # Enable pre-commit hook
pre-commit install --hook-type commit-msg  # Enable commit message linting
```

### Running Manually

```bash
# Run without ninja (non-macOS)
./Build/release/bin/Ladybird

# Run on macOS
open -W --stdout $(tty) --stderr $(tty) ./Build/release/bin/Ladybird.app

# With arguments on macOS
open -W --stdout $(tty) --stderr $(tty) ./Build/release/bin/Ladybird.app --args https://ladybird.dev
```

## Architecture

### Multi-Process Design

Ladybird uses separate processes for security and stability:

- **Browser/UI Process**: Main application (Qt on most platforms, AppKit on macOS, Android UI on Android)
- **WebContent**: One per tab, runs LibWeb (HTML/CSS engine) and LibJS (JavaScript engine)
- **RequestServer**: One per WebContent, handles network requests (HTTP/HTTPS)
- **ImageDecoder**: Spawned per image, decodes images in sandboxed environment
- **WebDriver**: For browser automation
- **WebWorker**: For Web Worker API support

### Directory Structure

- **AK/**: Application Kit - fundamental data structures and utilities
- **Base/**: Resource files (fonts, icons, themes)
- **Libraries/**: Core libraries
  - **LibWeb**: Web rendering engine
  - **LibJS**: JavaScript engine
  - **LibWasm**: WebAssembly implementation
  - **LibGfx**: 2D graphics, image decoding
  - **LibHTTP**: HTTP/1.1 client
  - **LibCore**: Event loop, OS abstraction
  - **LibIPC**: Inter-process communication (includes fork-specific security utilities)
  - **LibUnicode**: Unicode and locale support
  - **LibMedia**: Audio/video playback
  - **LibCrypto/LibTLS**: Cryptography and TLS
- **Services/**: Out-of-process services (WebContent, RequestServer, ImageDecoder, WebDriver, WebWorker)
- **UI/**: Platform-specific UI code (Qt, AppKit, Android)
- **Tests/**: Test suites (LibWeb tests, unit tests, IPC security tests)
- **Meta/**: Build scripts, code generation tools, linters, IPC compiler
- **Documentation/**: Comprehensive documentation
- **claudedocs/**: Fork-specific documentation (IPC security guides, Windows build instructions)

## Fork-Specific Security Components

### LibIPC Security Utilities

Located in `Libraries/LibIPC/`:

- **Limits.h**: Constants for IPC message size and rate limits (MAX_MESSAGE_SIZE, MAX_RATE_LIMIT, etc.)
- **RateLimiter.h**: Per-connection rate limiting with sliding window algorithm
- **SafeMath.h**: Overflow-safe arithmetic for dimension/size calculations
- **ValidatedDecoder.h**: Bounds-checked IPC message decoding with validation

### Usage Patterns

```cpp
// Rate limiting IPC handlers
RateLimiter m_rate_limiter{100, std::chrono::seconds(1)}; // 100 messages per second
if (!m_rate_limiter.check_and_update()) {
    return {}; // Rate limit exceeded, reject message
}

// Validated decoding with bounds checking
ValidatedDecoder validated(decoder);
auto page_id = TRY(validated.decode_validated<i32>(0, MAX_PAGE_ID));
auto url = TRY(validated.decode_validated_string(0, MAX_URL_LENGTH));

// Safe arithmetic to prevent overflows
using namespace IPC::SafeMath;
auto result = safe_multiply(width, height);
if (result.has_overflow()) {
    return Error::from_string_literal("Dimension overflow");
}
auto total_size = TRY(safe_add(base_size, header_size));
```

### IPC Fuzzing Framework

Located in `Meta/Lagom/Fuzzers/`:

- **FuzzIPC.cpp**: General IPC message fuzzing
- **FuzzWebContentIPC.cpp**: WebContent-specific IPC fuzzing
- Automated malformed message testing for security validation

### Fork Documentation

See `claudedocs/security-hardening/` for detailed implementation guides:
- **IPC-Security-Implementation.md**: Implementation details for IPC security features
- **Security-Guidelines.md**: Security best practices for this fork
- **Phase1-Implementation-Summary.md**: Summary of initial security enhancements
- **Windows-Build-Setup-Guide.md**: Detailed Windows-specific build instructions

See `claudedocs/` root for general build troubleshooting and solutions.

## Coding Standards

### Language and Style

- **C++23** is required; use modern C++ features and AK containers throughout
- Use **clang-format** for automatic formatting (see `Meta/lint-clang-format.py` for required version)
- Follow the project's **CodingStyle.md**:
  - **CamelCase** for classes, structs, namespaces
  - **snake_case** for functions and variables
  - **SCREAMING_CASE** for constants
  - Member variables: prefix with `m_` (instance), `s_` (static), `g_` (global)
  - Match spec algorithm names exactly when implementing web standards

### Error Handling

Use TRY/MUST patterns extensively:

```cpp
// TRY for error propagation (like Rust's ? operator)
ErrorOr<void> do_something() {
    auto result = TRY(fallible_operation());
    return {};
}

// MUST for operations that should never fail
MUST(vector.try_append(item));  // After ensuring capacity
```

### Common Patterns

- **Fallible constructors**: Use static `create()` methods returning `ErrorOr<T>`
- **Entry point**: Use `ladybird_main(Main::Arguments)` returning `ErrorOr<int>`, not regular `main()`
- **String literals**: Use `"text"sv` for `StringView` literals (zero runtime cost)
- **Collections**: Prefer `Vector` for dynamic arrays, `Array` for compile-time sized, `FixedArray` for runtime-sized immutable

## Commit Message Format

Strict format required:

```
Category: Brief imperative description

Detailed explanation if needed. Wrap at 72 characters.
```

**Category examples**: `LibWeb`, `LibJS`, `WebContent`, `RequestServer`, `AK`, `CI`
- Use component names, not generic names like "Libraries"
- Multiple categories: `LibJS+LibWeb+Browser: ...`
- Write in **imperative mood**: "Add feature" not "Added feature"
- **No period** at end of subject line

## Testing Policy

- Add tests when fixing bugs or adding features
- LibWeb tests in `Tests/LibWeb/` (Text, Layout, Ref, or Screenshot types)
- Create tests using: `./Tests/LibWeb/add_libweb_test.py your-test-name test_type`
- Import relevant Web Platform Tests when applicable
- CI runs with Address Sanitizer and Undefined Sanitizer
- For IPC security features: add tests in `Tests/LibIPC/` and fuzzers in `Meta/Lagom/Fuzzers/`

## Important Development Notes

### LibWeb Development

- When adding CSS properties: see `Documentation/CSSProperties.md`
- When adding IDL files: see `Documentation/AddNewIDLFile.md`
- Follow patterns in `Documentation/LibWebPatterns.md`
- Understand the rendering pipeline: `Documentation/LibWebFromLoadingToPainting.md`

### Process Architecture

- Each tab gets its own WebContent process
- WebContent spawns its own RequestServer and ImageDecoder processes
- All processes are sandboxed (pledge/unveil mechanisms)
- IPC is handled via LibIPC
- **Fork enhancement**: IPC messages include rate limiting and validated decoding

### IPC Security (Fork-Specific)

When working with IPC in this fork:
- Use `RateLimiter` for frequently-called handlers (e.g., mouse move, scroll events)
- Use `ValidatedDecoder` for decoding untrusted input from other processes
- Use `SafeMath` functions for any arithmetic on IPC-provided dimensions/sizes
- Consult `Limits.h` for appropriate constants (message sizes, rate limits, etc.)
- Add fuzzing tests for new IPC message handlers

### Human Language Policy

All user-facing strings, code comments, and commit messages:
- Use **American English** with ISO 8601 dates and metric units
- Use proper spelling, grammar, and punctuation
- Write in authoritative and technical tone
- Avoid contractions, slang, idioms, humor, and sarcasm
- Use gender-neutral pronouns

### Build Prerequisites

- C++23 compiler (gcc-14 or clang-20 recommended)
- CMake 3.25+
- Qt6 development packages (except on macOS with AppKit)
- nasm, ninja, and various build tools
- See `Documentation/BuildInstructionsLadybird.md` for platform-specific requirements
- **For Windows/WSL**: See `claudedocs/HOW_TO_BUILD_LADYBIRD_WINDOWS.md` and `claudedocs/LADYBIRD_BUILD_SOLUTIONS.md`

### What Not to Do

- Don't submit changes incompatible with 2-clause BSD license
- Don't touch code outside PR scope
- Don't use weasel-words like "refactor" without explaining changes
- Don't include commented-out code
- Don't write in C style; use C++ features and AK containers
- Don't add jokes to user-facing parts
- Don't attempt large architectural changes without familiarity with the codebase
- **Fork-specific**: Don't propose fork-specific security features for upstream contribution (they are experimental only)

# Testing Guide for Ladybird Fork

## Test Discovery Results

**Test Categories Found**:
- AK (Application Kit) tests: ~80+ unit tests
- LibWeb tests: Web rendering engine tests
- LibJS tests: JavaScript engine tests
- LibIPC tests: **Custom IPC compiler tests** (added in this fork)
- Fuzzer tests: IPC fuzzing framework (added in this fork)

## Build Status

**Current State**: Project not yet built
**Required**: Build the project before running tests

## How to Build and Test

### 1. Initial Build

```bash
# Build with default Release preset
./Meta/ladybird.py run

# Or build with Debug preset (recommended for testing)
BUILD_PRESET=Debug ./Meta/ladybird.py run
```

### 2. Run All Tests

```bash
# Run complete test suite
./Meta/ladybird.py test

# Run tests with output on failure
CTEST_OUTPUT_ON_FAILURE=1 ninja -C Build/release test
```

### 3. Run Specific Test Categories

```bash
# LibWeb tests only
./Meta/ladybird.py test LibWeb

# AK (Application Kit) tests
./Meta/ladybird.py test AK

# LibJS tests
./Meta/ladybird.py test LibJS

# Custom IPC tests (added in this fork)
./Meta/ladybird.py test LibIPC
```

### 4. Run Custom IPC Tests

**IPC Compiler Tests**:
```bash
# Build and run IPC compiler validation tests
cd Build/release
ninja TestIPCCompiler
./bin/TestIPCCompiler
```

**IPC Fuzzing Tests**:
```bash
# Build fuzzing tests
cd Build/release
ninja FuzzIPC FuzzWebContentIPC

# Run IPC fuzzer
./bin/FuzzIPC

# Run WebContent IPC fuzzer
./bin/FuzzWebContentIPC
```

### 5. Web Platform Tests

```bash
# Run WPT tests
./Meta/WPT.sh run --log results.log

# Compare results
./Meta/WPT.sh compare --log results.log expectations.log css

# Import specific WPT tests
./Meta/WPT.sh import html/dom/aria-attribute-reflection.html
```

## Test Types

### Unit Tests (CTest)
- **Location**: `Tests/*/`
- **Framework**: CTest (CMake's testing framework)
- **Execution**: Compiled C++ test executables

### LibWeb Tests
- **Location**: `Tests/LibWeb/`
- **Types**:
  - Text tests (HTML parsing and rendering)
  - Layout tests (CSS layout engine)
  - Ref tests (visual comparison)
  - Screenshot tests (visual regression)

### Fuzzing Tests (Custom)
- **Location**: `Meta/Lagom/Fuzzers/`
- **Framework**: libFuzzer
- **Custom Fuzzers**:
  - `FuzzIPC.cpp` - General IPC message fuzzing
  - `FuzzWebContentIPC.cpp` - WebContent-specific IPC fuzzing

## Test Coverage

### Standard Ladybird Tests
- Application Kit (AK): Data structures and utilities
- LibWeb: HTML/CSS rendering engine
- LibJS: JavaScript engine
- LibWasm: WebAssembly implementation
- LibGfx: Graphics and image decoding
- LibHTTP: HTTP client
- LibCore: Event loop and OS abstraction

### Custom Fork Tests (Added)
- **LibIPC Tests**: IPC compiler validation and code generation
- **IPC Fuzzers**: Automated security testing for IPC messages
- **Validation Tests**: Rate limiting, bounds checking, overflow protection

## Running Tests in CI/CD Mode

### Quick Test Run
```bash
# Fast sanity check
./Meta/ladybird.py test --quick
```

### Full Test Suite with Sanitizers
```bash
# Build with sanitizers
cmake --preset Sanitizers
cmake --build --preset Sanitizers

# Run tests with ASAN and UBSAN
cd Build/sanitizers
CTEST_OUTPUT_ON_FAILURE=1 ninja test
```

### Coverage Analysis
```bash
# Build with coverage instrumentation
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON -B Build/coverage
cmake --build Build/coverage

# Run tests
cd Build/coverage
ninja test

# Generate coverage report
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

## Test Output Examples

### Successful Test Run
```
Test project /path/to/Build/release
    Start 1: TestAllOf
1/150 Test #1: TestAllOf ........................   Passed    0.01 sec
    Start 2: TestAnyOf
2/150 Test #2: TestAnyOf ........................   Passed    0.01 sec
...
100% tests passed, 0 tests failed out of 150
```

### Failed Test with Details
```
Test project /path/to/Build/release
    Start 42: TestIPCCompiler
42/150 Test #42: TestIPCCompiler ..................***Failed    0.05 sec

Test output:
FAIL: Validation attribute generation
Expected: @validate(range: 0, 100)
Got: (none)
```

## Debugging Failed Tests

### Run Individual Test
```bash
# Run specific test binary directly
./Build/release/bin/TestIPCCompiler

# With gdb debugger
gdb ./Build/release/bin/TestIPCCompiler
```

### Verbose Test Output
```bash
# Show all test output
CTEST_OUTPUT_ON_FAILURE=1 ninja -C Build/release test

# Or run with verbose flag
./Meta/ladybird.py test --verbose
```

### Rebaseline Web Tests
```bash
# Update expected output for specific test
./Meta/ladybird.py run test-web --rebaseline -f Text/input/your-test.html
```

## Test Development Workflow

### Adding New Tests

1. **Create test file** in appropriate directory:
   ```cpp
   // Tests/LibIPC/TestNewFeature.cpp
   #include <LibTest/TestCase.h>

   TEST_CASE(new_feature_works) {
       EXPECT_EQ(actual, expected);
   }
   ```

2. **Register test** in CMakeLists.txt:
   ```cmake
   lagom_test(TestNewFeature.cpp)
   ```

3. **Build and run**:
   ```bash
   ninja -C Build/release TestNewFeature
   ./Build/release/bin/TestNewFeature
   ```

### Adding LibWeb Tests

```bash
# Create new LibWeb test
./Tests/LibWeb/add_libweb_test.py my-new-test Text

# Test created at: Tests/LibWeb/Text/input/my-new-test.html
```

## Continuous Testing

### Watch Mode (Development)
```bash
# Terminal 1: Watch for file changes
watch -n 2 'cmake --build Build/release'

# Terminal 2: Continuously run tests
watch -n 5 'ninja -C Build/release test'
```

### Pre-commit Testing
```bash
# Install pre-commit hooks
pre-commit install

# Manually run pre-commit checks
pre-commit run --all-files
```

## Performance Benchmarking

### Test Execution Time
```bash
# Time all tests
time ninja -C Build/release test

# Profile individual test
time ./Build/release/bin/TestIPCCompiler
```

### Memory Usage Analysis
```bash
# Run tests with valgrind
valgrind --leak-check=full ./Build/release/bin/TestIPCCompiler
```

## Test Maintenance

### Update Test Expectations
```bash
# Rebaseline all LibWeb tests
./Meta/ladybird.py run test-web --rebaseline

# Rebaseline specific category
./Meta/ladybird.py run test-web --rebaseline -f Text/
```

### Clean Test Artifacts
```bash
# Remove test build artifacts
rm -rf Build/release/Tests/

# Full clean and rebuild
rm -rf Build/
./Meta/ladybird.py run
```

## Known Issues and Limitations

### Custom IPC Tests
- **Status**: Test files created but not yet built
- **Reason**: Requires full project build with CMake configuration
- **Solution**: Run `./Meta/ladybird.py run` first

### Fuzzing Tests
- **Requirements**: libFuzzer support in compiler
- **Platform**: May require specific compiler flags on Windows/WSL
- **Execution**: Long-running tests, use timeout controls

### Platform-Specific Tests
- **macOS**: Some tests require AppKit
- **Linux**: Some tests require X11/Wayland
- **Windows/WSL**: May require WSL2 with graphics support

## Next Steps

1. **Build Project**: `./Meta/ladybird.py run`
2. **Run Tests**: `./Meta/ladybird.py test`
3. **Check Custom Tests**: `./Meta/ladybird.py test LibIPC`
4. **Run Fuzzers**: Build and execute IPC fuzzing tests
5. **Review Results**: Check for failures and coverage gaps

## Resources

- **Upstream Testing Docs**: `Documentation/Testing.md`
- **LibWeb Test Guide**: `Tests/LibWeb/README.md`
- **Build Instructions**: `Documentation/BuildInstructionsLadybird.md`
- **Custom IPC Tests**: `Tests/LibIPC/TestIPCCompiler.cpp`
- **Fuzzing Framework**: `Meta/Lagom/Fuzzers/`

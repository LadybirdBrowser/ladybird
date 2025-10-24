# Ladybird Browser - Personal Development Fork

This is a personal fork of the [Ladybird Browser](https://github.com/LadybirdBrowser/ladybird) project for learning, experimentation, and development purposes.

## Fork Status

- **Upstream**: Synchronized with [LadybirdBrowser/ladybird](https://github.com/LadybirdBrowser/ladybird)
- **Purpose**: Personal learning and IPC security research
- **Contribution Policy**: This fork contains experimental features not intended for upstream contribution

## Custom Additions

This fork includes experimental IPC security enhancements and development infrastructure that extend the base Ladybird browser for research and learning purposes.

### Security Enhancements

**LibIPC Security Features** (`Libraries/LibIPC/`):
- `Limits.h` - IPC message size and rate limiting constants
- `RateLimiter.h` - Per-connection message rate limiting with sliding window algorithm
- `SafeMath.h` - Overflow-safe arithmetic operations for IPC value validation
- `ValidatedDecoder.h` - Bounds-checked IPC message decoding with validation

**Service Process Hardening** (`Services/`):
- **WebContent** - Enhanced page_id validation and input sanitization
- **ImageDecoder** - Resource limit enforcement and validated decoding
- **RequestServer** - URL length validation and request rate limiting

**IPC Compiler Enhancements** (`Meta/Tools/CodeGenerators/IPCCompiler/`):
- Automatic validation attribute generation
- Type-safe parameter bounds checking
- Security annotation support

### Testing and Fuzzing

**IPC Fuzzing Framework** (`Meta/Lagom/Fuzzers/`):
- `FuzzIPC.cpp` - General IPC message fuzzing
- `FuzzWebContentIPC.cpp` - WebContent-specific IPC fuzzing
- Automated malformed message testing

**IPC Compiler Tests** (`Tests/LibIPC/`):
- `TestIPCCompiler.cpp` - Validation attribute generation tests
- Type safety verification
- Edge case handling validation

### Development Infrastructure

**Documentation** (`claudedocs/`):
- IPC security implementation guides
- Validation attribute usage documentation
- Migration examples and patterns
- Windows build setup guides
- Comprehensive security analysis reports

**AI-Assisted Development**:
- `CLAUDE.md` - Claude Code integration guide with project context
- Development workflow documentation
- Architectural guidance for AI-assisted contributions

## Documentation Structure

```
claudedocs/
├── TODO_ANALYSIS.md                    # Development roadmap and contribution strategy
└── security-hardening/
    ├── Comprehensive-Security-Analysis-Report.md
    ├── IPC-Validation-Attributes-Guide.md
    ├── ValidatedDecoder-Usage-Guide.md
    ├── WebContentClient-Security-Analysis.md
    ├── ServiceProcesses-Security-Analysis.md
    ├── Migration-Example.md
    ├── Windows-Build-Setup-Guide.md
    └── [Weekly Progress Reports]
```

## Security Features Overview

### Rate Limiting
```cpp
// Example: Rate-limited IPC handler
RateLimiter m_rate_limiter{100, std::chrono::seconds(1)}; // 100 msg/sec

Messages::WebContentServer::HandleMouseMoveResponse handle_mouse_move(i32 page_id, Gfx::IntPoint position) {
    if (!m_rate_limiter.check_and_update()) {
        return {}; // Rate limit exceeded
    }
    // Process message...
}
```

### Validated Decoding
```cpp
// Example: Bounds-checked decoding
ErrorOr<void> decode_with_validation(IPC::Decoder& decoder) {
    ValidatedDecoder validated(decoder);
    auto page_id = TRY(validated.decode_validated<i32>(0, MAX_PAGE_ID));
    auto url = TRY(validated.decode_validated_string(0, MAX_URL_LENGTH));
    return {};
}
```

### SafeMath Operations
```cpp
// Example: Overflow-safe arithmetic
using namespace IPC::SafeMath;
auto result = safe_multiply(width, height);
if (result.has_overflow()) {
    return Error::from_string_literal("Dimension overflow");
}
```

## Building and Testing

### Standard Build
```bash
# Use upstream build instructions
./Meta/ladybird.py run
```

### Security Testing
```bash
# Run IPC fuzzing tests
./Build/release/bin/FuzzIPC
./Build/release/bin/FuzzWebContentIPC

# Run IPC compiler validation tests
./Meta/ladybird.py test LibIPC
```

### Windows Build Setup
See `claudedocs/security-hardening/Windows-Build-Setup-Guide.md` for detailed Windows-specific build instructions.

## Sync Strategy

This fork regularly synchronizes with upstream:
```bash
git fetch upstream
git merge upstream/master
git push origin master
```

No feature branches are maintained to avoid "contribute to upstream" prompts.

## Learning Focus Areas

1. **IPC Security Architecture** - Understanding multi-process browser security
2. **Memory Safety** - Implementing bounds checking and overflow protection
3. **Fuzzing Techniques** - Automated security testing methodologies
4. **Browser Architecture** - Multi-process design patterns
5. **C++23 Features** - Modern C++ in large-scale projects

## Disclaimer

This fork contains experimental security enhancements for educational purposes. The implementations:
- Are not security-audited
- May contain bugs or vulnerabilities
- Should not be used in production environments
- Are intended for learning and research only

For production use, please use the official [Ladybird Browser](https://github.com/LadybirdBrowser/ladybird).

## Upstream Project

Ladybird is a truly independent web browser with a novel engine based on web standards. For more information about the upstream project:

- Website: https://ladybird.org
- Repository: https://github.com/LadybirdBrowser/ladybird
- Discord: https://discord.gg/nvfjVJ4Svh

## License

This fork maintains the same 2-clause BSD license as the upstream Ladybird project. See LICENSE for details.

All custom additions in this fork are also released under the 2-clause BSD license.

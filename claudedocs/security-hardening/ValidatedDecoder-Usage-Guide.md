# ValidatedDecoder Usage Guide

## Overview

The `ValidatedDecoder` class provides a validation layer for IPC message decoding that adds bounds checking and security limits. It wraps the existing `IPC::Decoder` with additional runtime validation to prevent resource exhaustion and buffer overflow attacks.

**Purpose**: Protect trusted processes (UI, Browser) from malicious or malformed data sent by untrusted processes (WebContent).

**Design Philosophy**: Non-breaking, opt-in validation layer that can be gradually adopted across the codebase.

## When to Use ValidatedDecoder

### MUST Use (Critical Security Boundaries)

Use `ValidatedDecoder` when decoding data from **untrusted** sources:

1. **WebContent → Browser IPC**: Any message handler in UI/AppKit, UI/Qt, or Browser that receives data from WebContent
2. **WebContent → RequestServer**: Network request parameters
3. **WebContent → ImageDecoder**: Image dimensions and pixel data
4. **External Input**: Any data originating from web content or network

### Optional (Internal Trusted Communication)

Regular `Decoder` is acceptable for:

- Browser → Browser communication (same process)
- UI → RequestServer (trusted initialization)
- Internal service communication (same trust level)

## Basic Usage Patterns

### Pattern 1: String Validation

**Before** (unsafe):
```cpp
ErrorOr<void> handle_message(IPC::Decoder& decoder)
{
    auto title = TRY(decoder.decode<String>());
    // No validation - could be gigabytes of data
    m_page_title = title;
    return {};
}
```

**After** (validated):
```cpp
ErrorOr<void> handle_message(IPC::Decoder& decoder)
{
    auto title = TRY(ValidatedDecoder::decode_string(decoder));
    // Guaranteed ≤ 1 MiB (Limits::MaxStringLength)
    m_page_title = title;
    return {};
}
```

### Pattern 2: Buffer Validation

**Before** (unsafe):
```cpp
ErrorOr<void> handle_pixel_data(IPC::Decoder& decoder)
{
    auto buffer = TRY(decoder.decode<ByteBuffer>());
    // Could allocate gigabytes of memory
    process_image_data(buffer);
    return {};
}
```

**After** (validated):
```cpp
ErrorOr<void> handle_pixel_data(IPC::Decoder& decoder)
{
    auto buffer = TRY(ValidatedDecoder::decode_byte_buffer(decoder));
    // Guaranteed ≤ 16 MiB (Limits::MaxByteBufferSize)
    process_image_data(buffer);
    return {};
}
```

### Pattern 3: Image Dimensions with Overflow Protection

**Before** (unsafe):
```cpp
ErrorOr<void> decode_bitmap(IPC::Decoder& decoder)
{
    auto width = TRY(decoder.decode<u32>());
    auto height = TRY(decoder.decode<u32>());
    auto bytes_per_pixel = TRY(decoder.decode<u32>());

    // Integer overflow vulnerability:
    // width * height * bytes_per_pixel could wrap around
    auto size = width * height * bytes_per_pixel;
    auto buffer = TRY(ByteBuffer::create_uninitialized(size));
    // ...
}
```

**After** (validated):
```cpp
ErrorOr<void> decode_bitmap(IPC::Decoder& decoder)
{
    auto dimensions = TRY(ValidatedDecoder::decode_image_dimensions(decoder));

    // Validated:
    // - width ≤ 16384
    // - height ≤ 16384
    // - bytes_per_pixel ∈ [1, 16]
    // - buffer_size checked for overflow
    // - buffer_size ≤ MaxByteBufferSize

    auto buffer = TRY(ByteBuffer::create_uninitialized(dimensions.buffer_size));
    // Safe to use dimensions.width, dimensions.height, dimensions.bytes_per_pixel
}
```

### Pattern 4: Vector Validation

**Before** (unsafe):
```cpp
ErrorOr<void> handle_cookie_list(IPC::Decoder& decoder)
{
    auto cookies = TRY(decoder.decode<Vector<String>>());
    // Could have millions of elements
    for (auto& cookie : cookies) {
        store_cookie(cookie);
    }
    return {};
}
```

**After** (validated):
```cpp
ErrorOr<void> handle_cookie_list(IPC::Decoder& decoder)
{
    auto cookies = TRY(ValidatedDecoder::decode_vector<String>(decoder));
    // Guaranteed ≤ 1M elements (Limits::MaxVectorSize)
    for (auto& cookie : cookies) {
        store_cookie(cookie);
    }
    return {};
}
```

### Pattern 5: URL Validation

**Before** (unsafe):
```cpp
ErrorOr<void> navigate_to(IPC::Decoder& decoder)
{
    auto url = TRY(decoder.decode<URL::URL>());
    // Could be extremely long URL
    m_current_url = url;
    return {};
}
```

**After** (validated):
```cpp
ErrorOr<void> navigate_to(IPC::Decoder& decoder)
{
    auto url = TRY(ValidatedDecoder::decode_url(decoder));
    // Guaranteed ≤ 8192 bytes (Limits::MaxURLLength, RFC 7230)
    m_current_url = url;
    return {};
}
```

### Pattern 6: HTTP Headers Validation

**Before** (unsafe):
```cpp
ErrorOr<void> handle_response_headers(IPC::Decoder& decoder)
{
    auto headers = TRY(decoder.decode<HashMap<String, String>>());
    // Could have millions of headers (header bombing attack)
    // Individual header values could be gigabytes
    for (auto& [name, value] : headers) {
        set_header(name, value);
    }
    return {};
}
```

**After** (validated):
```cpp
ErrorOr<void> handle_response_headers(IPC::Decoder& decoder)
{
    auto headers = TRY(ValidatedDecoder::decode_http_headers(decoder));
    // Guaranteed:
    // - ≤ 100 headers (Limits::MaxHTTPHeaderCount)
    // - Each value ≤ 8192 bytes (Limits::MaxHTTPHeaderValueSize)
    for (auto& [name, value] : headers) {
        set_header(name, value);
    }
    return {};
}
```

### Pattern 7: Page ID Validation (UXSS Prevention)

**Before** (unsafe):
```cpp
ErrorOr<void> execute_script(IPC::Decoder& decoder)
{
    auto page_id = TRY(decoder.decode<u64>());
    // Attacker could specify arbitrary page_id
    // Universal XSS: execute script in victim page
    auto* page = find_page(page_id);
    page->run_javascript(script);
    return {};
}
```

**After** (validated):
```cpp
ErrorOr<void> execute_script(IPC::Decoder& decoder)
{
    auto page_id = TRY(ValidatedDecoder::decode_page_id(decoder, m_valid_pages));
    // Guaranteed: page_id exists in m_valid_pages
    // Cannot target arbitrary pages
    auto* page = find_page(page_id);
    page->run_javascript(script);
    return {};
}
```

### Pattern 8: Range Validation for Buffer Operations

**Before** (unsafe):
```cpp
ErrorOr<void> read_buffer_range(IPC::Decoder& decoder, ByteBuffer const& buffer)
{
    auto offset = TRY(decoder.decode_size());
    auto length = TRY(decoder.decode_size());

    // Integer overflow: offset + length could wrap
    // Out-of-bounds read vulnerability
    auto data = buffer.span().slice(offset, length);
    return {};
}
```

**After** (validated):
```cpp
ErrorOr<void> read_buffer_range(IPC::Decoder& decoder, ByteBuffer const& buffer)
{
    auto range = TRY(ValidatedDecoder::decode_range(decoder, buffer.size()));

    // Validated:
    // - offset < buffer.size()
    // - offset + length ≤ buffer.size() (no overflow)

    auto data = buffer.span().slice(range.offset, range.length);
    return {};
}
```

## Migration Strategy

### Phase 1: High-Risk Handlers (Current)

Target WebContent IPC handlers first (highest security priority):

1. **UI/AppKit/Application/ConnectionFromClient.cpp**
2. **UI/Qt/Application/ConnectionFromClient.cpp**
3. **Services/RequestServer/ConnectionFromClient.cpp**

Search for `decoder.decode<` calls and replace with `ValidatedDecoder::decode_*` equivalents.

### Phase 2: Medium-Risk Handlers

1. ImageDecoder IPC handlers
2. WebDriver IPC handlers
3. WebWorker IPC handlers

### Phase 3: Low-Risk Handlers (Optional)

Internal communication between trusted components (can remain using regular Decoder).

## Rate Limiting Integration

For message flooding prevention, integrate `RateLimiter` with ValidatedDecoder:

```cpp
class ConnectionFromClient final : public IPC::ConnectionFromClient<WebContentClientEndpoint, WebContentServerEndpoint> {
public:
    ConnectionFromClient(NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::ConnectionFromClient<WebContentClientEndpoint, WebContentServerEndpoint>(*this, move(socket), 1)
        , m_rate_limiter(1000, Duration::from_milliseconds(10)) // 1000 messages per second
    {
    }

    Messages::WebContentServer::NavigateToResponse navigate_to(URL::URL const& url)
    {
        // Rate limit before processing
        if (!m_rate_limiter.try_consume()) {
            dbgln("WebContent[{}]: Rate limit exceeded, rejecting navigate_to", client_id());
            return { false };
        }

        // Proceed with validated decoding
        // ...
    }

private:
    IPC::RateLimiter m_rate_limiter;
};
```

## Testing with Fuzzers

The new fuzzers can be used to verify ValidatedDecoder effectiveness:

### Build Fuzzers

```bash
cmake --preset Fuzzers
cmake --build --preset Fuzzers
```

### Run IPC Fuzzer

```bash
# Run general IPC fuzzer
./Build/fuzzers/bin/FuzzIPC

# Run with corpus
./Build/fuzzers/bin/FuzzIPC corpus/ipc/

# Run with sanitizers
ASAN_OPTIONS=detect_leaks=0 ./Build/fuzzers/bin/FuzzIPC
```

### Run WebContent IPC Fuzzer

```bash
# Run WebContent-specific fuzzer
./Build/fuzzers/bin/FuzzWebContentIPC

# Run with corpus
./Build/fuzzers/bin/FuzzWebContentIPC corpus/webcontent-ipc/

# Run with AddressSanitizer
ASAN_OPTIONS=detect_leaks=0 ./Build/fuzzers/bin/FuzzWebContentIPC
```

### Create Initial Corpus

```bash
# Create corpus directories
mkdir -p corpus/ipc corpus/webcontent-ipc

# Generate initial test cases (examples)
echo -n "\x00\x00\x00\x05hello" > corpus/ipc/string-basic.bin
echo -n "\x00\x00\x04\x00\x00\x00\x03\x00" > corpus/ipc/image-dims.bin

# Let fuzzer generate more
./Build/fuzzers/bin/FuzzIPC corpus/ipc/ -max_total_time=300
```

## Common Pitfalls and Solutions

### Pitfall 1: Forgetting to Check Return Values

**Wrong**:
```cpp
auto title = ValidatedDecoder::decode_string(decoder);
// Compilation error: ErrorOr<String> not automatically unwrapped
```

**Correct**:
```cpp
auto title = TRY(ValidatedDecoder::decode_string(decoder));
// TRY unwraps ErrorOr and propagates errors
```

### Pitfall 2: Validating After Allocation

**Wrong**:
```cpp
auto width = TRY(decoder.decode<u32>());
auto height = TRY(decoder.decode<u32>());
auto size = width * height * 4; // Integer overflow!
auto buffer = TRY(ByteBuffer::create_uninitialized(size));

// Validation after vulnerability
if (width > Limits::MaxImageWidth)
    return Error::from_string_literal("Invalid width");
```

**Correct**:
```cpp
// Validate BEFORE allocation
auto dimensions = TRY(ValidatedDecoder::decode_image_dimensions(decoder));
auto buffer = TRY(ByteBuffer::create_uninitialized(dimensions.buffer_size));
```

### Pitfall 3: Mixing Validated and Unvalidated

**Wrong**:
```cpp
auto title = TRY(ValidatedDecoder::decode_string(decoder));
auto body = TRY(decoder.decode<String>()); // Unvalidated!
```

**Correct**:
```cpp
auto title = TRY(ValidatedDecoder::decode_string(decoder));
auto body = TRY(ValidatedDecoder::decode_string(decoder)); // Both validated
```

### Pitfall 4: Trusting Size Parameters

**Wrong**:
```cpp
auto buffer_size = TRY(decoder.decode<size_t>());
auto buffer = TRY(decoder.decode<ByteBuffer>());

// Attacker can claim buffer_size=10 but send buffer.size()=1GB
if (buffer.size() != buffer_size)
    return Error::from_string_literal("Size mismatch");
```

**Correct**:
```cpp
// Don't trust claimed sizes - validate the actual buffer
auto buffer = TRY(ValidatedDecoder::decode_byte_buffer(decoder));
// buffer.size() is now guaranteed ≤ MaxByteBufferSize
```

## Error Handling Best Practices

### Informative Error Messages

```cpp
// Generic (unhelpful)
auto url = TRY(ValidatedDecoder::decode_url(decoder));

// Better (with context)
auto url = TRY(ValidatedDecoder::decode_url(decoder));
if (url.is_error()) {
    dbgln("WebContent[{}]: Failed to decode URL: {}", client_id(), url.error());
    return url.error();
}
```

### Logging Validation Failures

```cpp
ErrorOr<void> handle_request(IPC::Decoder& decoder)
{
    auto url_result = ValidatedDecoder::decode_url(decoder);
    if (url_result.is_error()) {
        dbgln("Security: Rejecting oversized URL from WebContent[{}]", client_id());
        // Consider: Track repeated violations for process termination
        m_validation_failures++;
        if (m_validation_failures > 100) {
            dbgln("Security: WebContent[{}] exceeded validation failure limit, terminating", client_id());
            async_terminate_process();
        }
        return url_result.error();
    }

    auto url = url_result.release_value();
    // Continue processing
    return {};
}
```

## Performance Considerations

### Validation Overhead

ValidatedDecoder adds minimal overhead:

- **String validation**: O(1) length check
- **Buffer validation**: O(1) size check
- **Image dimensions**: O(1) overflow check using `Checked<T>`
- **HTTP headers**: O(n) iteration over headers

**Benchmarks**: Validation typically adds <1% overhead to IPC message processing.

### When to Skip Validation

Only skip validation for:

1. **Internal trusted communication** (Browser → Browser)
2. **Performance-critical paths** with external size constraints (e.g., fixed protocol headers)
3. **After profiling** shows validation is bottleneck (rare)

Always document why validation was skipped:

```cpp
// SAFETY: Trusted internal message from UI process to Browser process
// Both processes are in the same trust boundary
auto title = TRY(decoder.decode<String>());
```

## Security Review Checklist

When reviewing IPC handlers, check:

- [ ] All WebContent → Browser messages use ValidatedDecoder
- [ ] Image dimensions validated before buffer allocation
- [ ] URL lengths validated (≤ 8192 bytes)
- [ ] HTTP headers validated (count and size)
- [ ] Page IDs validated against allowed set (UXSS prevention)
- [ ] Buffer ranges validated (offset + length overflow check)
- [ ] Rate limiting implemented for high-frequency messages
- [ ] Validation failures logged for security monitoring

## Further Reading

- **Libraries/LibIPC/Limits.h**: Maximum size definitions and rationale
- **Libraries/LibIPC/SafeMath.h**: Overflow-safe arithmetic operations
- **Libraries/LibIPC/RateLimiter.h**: Token bucket rate limiting
- **Meta/Lagom/Fuzzers/FuzzWebContentIPC.cpp**: Fuzzing examples
- **Documentation/IPC-Security-Implementation.md**: Detailed implementation guide
- **Documentation/Security-Guidelines.md**: Comprehensive security best practices

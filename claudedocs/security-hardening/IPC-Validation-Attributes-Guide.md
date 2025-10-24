# IPC Validation Attributes - Developer Guide

**Purpose**: Comprehensive guide for using validation attributes in .ipc files to automatically generate security validation code.

**Target Audience**: Ladybird developers adding new IPC messages or updating existing endpoints.

---

## Quick Start

### Basic Example

```cpp
endpoint MyService
{
    [RateLimited]
    process_data(
        [MaxLength=256] ByteString name,
        [MaxSize=1000] Vector<u32> values
    ) =|
}
```

**What this generates**:
1. `decode()` method with automatic length/size validation
2. `handle()` dispatcher with rate limiting check
3. Clear error messages for validation failures

**What you DON'T need to write**:
- Manual `validate_string_length()` calls
- Manual `validate_vector_size()` calls
- Manual `check_rate_limit()` calls
- Custom error handling for validation failures

---

## Attribute Reference

### Parameter-Level Attributes

#### [MaxLength=N]

**Applies to**: `String`, `ByteString`, `Utf16String`

**Purpose**: Limits maximum byte length of strings to prevent memory exhaustion.

**Syntax**:
```cpp
[MaxLength=256] ByteString field_name
[MaxLength=8192] String long_field
```

**Generated Validation**:
```cpp
// For ByteString/String:
if (field_name.bytes_as_string_view().length() > 256)
    return Error::from_string_literal("Decoded field_name exceeds maximum length");

// For Utf16String:
if (field_name.to_utf8().bytes_as_string_view().length() > 256)
    return Error::from_string_literal("Decoded field_name exceeds maximum length");
```

**Recommended Values**:
- Short identifiers: `MaxLength=64`
- Filenames/paths: `MaxLength=256`
- HTTP methods: `MaxLength=16`
- URLs (see AllowedSchemes): `MaxLength=8192`
- Request bodies (see MaxSize for ByteBuffer): Not applicable

**Attack Prevented**: Memory exhaustion via enormous strings

---

#### [MaxSize=N]

**Applies to**: `Vector<T>`, `ByteBuffer`, `HTTP::HeaderMap`

**Purpose**: Limits collection size or buffer byte count to prevent resource exhaustion.

**Syntax**:
```cpp
[MaxSize=100] Vector<ByteString> protocols
[MaxSize=10000] HTTP::HeaderMap headers
[MaxSize=104857600] ByteBuffer image_data  // 100MB
```

**Generated Validation**:
```cpp
// For Vector<T>:
if (protocols.size() > 100)
    return Error::from_string_literal("Decoded protocols exceeds maximum size");

// For ByteBuffer:
if (image_data.size() > 104857600)
    return Error::from_string_literal("Decoded image_data exceeds maximum buffer size");

// For HTTP::HeaderMap:
if (headers.headers().size() > 10000)
    return Error::from_string_literal("Decoded headers exceeds maximum header count");
```

**Recommended Values**:
- Small collections (protocols, extensions): `MaxSize=100`
- HTTP headers: `MaxSize=10000`
- Image buffers: `MaxSize=104857600` (100MB)
- Large data buffers: `MaxSize=1073741824` (1GB, use sparingly)

**Attack Prevented**: Vector/buffer exhaustion DoS attacks

---

#### [AllowedSchemes("scheme1","scheme2")]

**Applies to**: `URL::URL`

**Purpose**: Whitelist allowed URL schemes to prevent SSRF attacks.

**Syntax**:
```cpp
[AllowedSchemes("http","https")] URL::URL url
[AllowedSchemes("ws","wss")] URL::URL websocket_url
```

**Generated Validation**:
```cpp
if (!url.scheme().is_one_of("http"sv, "https"sv))
    return Error::from_string_literal("Decoded url has disallowed URL scheme");
```

**Recommended Values**:
- HTTP requests: `AllowedSchemes("http","https")`
- WebSocket connections: `AllowedSchemes("ws","wss")`
- Data URIs (avoid if possible): `AllowedSchemes("data")`

**NEVER Allow**:
- `file://` - Local file access (SSRF to read `/etc/passwd`, etc.)
- `javascript://` - XSS vector
- `data://` - Embedded content (can bypass CSP)
- Internal schemes without explicit security review

**Attack Prevented**: SSRF (Server-Side Request Forgery) via malicious URL schemes

---

#### [NoCRLF]

**Applies to**: `String`, `ByteString`, `HTTP::HeaderMap`

**Purpose**: Reject strings/headers containing `\r` or `\n` to prevent CRLF injection attacks.

**Syntax**:
```cpp
[NoCRLF] ByteString http_method
[NoCRLF] String header_value
[NoCRLF] HTTP::HeaderMap request_headers
```

**Generated Validation**:
```cpp
// For String/ByteString:
if (http_method.contains('\r') || http_method.contains('\n'))
    return Error::from_string_literal("Decoded http_method contains CRLF characters");

// For HTTP::HeaderMap:
for (auto const& header : request_headers.headers()) {
    if (header.name.contains('\r') || header.name.contains('\n') ||
        header.value.contains('\r') || header.value.contains('\n'))
        return Error::from_string_literal("Decoded request_headers contains CRLF in headers");
}
```

**Use Cases**:
- HTTP methods: `[NoCRLF] ByteString method`
- HTTP header values: `[NoCRLF] ByteString user_agent`
- All HTTP::HeaderMap parameters: `[NoCRLF] HTTP::HeaderMap headers`
- Any string used in protocol construction

**Attack Prevented**: CRLF injection for HTTP request smuggling

---

### Message-Level Attributes

#### [RateLimited]

**Applies to**: Entire message handler

**Purpose**: Enforce rate limiting (1000 messages/second via token bucket) at handler dispatch.

**Syntax**:
```cpp
[RateLimited]
message_name(parameters...) =|
```

**Generated Validation**:
```cpp
// In handle() dispatcher:
case (int)Messages::MyService::MessageID::MessageName: {
    if (!check_rate_limit())
        return Error::from_string_literal("Rate limit exceeded for message_name");
    // ... handler invocation
}
```

**Requirement**: Endpoint class must provide `check_rate_limit()` method:
```cpp
class ConnectionFromClient : public IPC::ConnectionFromClient<...> {
    IPC::RateLimiter m_rate_limiter { 1000, Duration::from_milliseconds(10) };

    [[nodiscard]] bool check_rate_limit() {
        return m_rate_limiter.try_consume();
    }
};
```

**Apply to**:
- All handlers accepting untrusted WebContent input
- High-frequency handlers (decode_image, start_request)
- Handlers with expensive operations

**Attack Prevented**: Message flood DoS attacks

---

## Combining Attributes

### Multiple Attributes on Same Parameter

```cpp
// String validation: length limit + CRLF prevention
[MaxLength=256, NoCRLF] ByteString http_method

// URL validation: scheme whitelist + length limit
[AllowedSchemes("http","https"), MaxLength=8192] URL::URL url

// Header validation: size limit + CRLF prevention
[MaxSize=10000, NoCRLF] HTTP::HeaderMap request_headers
```

**Validation Order**: All validations execute sequentially in the order listed in generated code.

---

## Complete Endpoint Example

```cpp
endpoint RequestServer
{
    // High-risk handler with full validation
    [RateLimited]
    start_request(
        i32 request_id,
        [MaxLength=256, NoCRLF] ByteString method,
        [AllowedSchemes("http","https"), MaxLength=8192] URL::URL url,
        [MaxSize=10000, NoCRLF] HTTP::HeaderMap request_headers,
        [MaxLength=104857600] ByteBuffer request_body,  // 100MB limit
        Core::ProxyData proxy_data
    ) =|

    // WebSocket with protocol/extension limits
    [RateLimited]
    websocket_connect(
        i32 websocket_id,
        [AllowedSchemes("ws","wss"), MaxLength=8192] URL::URL url,
        [MaxLength=256] ByteString origin,
        [MaxSize=100] Vector<ByteString> protocols,
        [MaxSize=100] Vector<ByteString> extensions,
        [MaxSize=10000, NoCRLF] HTTP::HeaderMap request_headers
    ) =|

    // Simple string validation only
    [RateLimited]
    set_dns_server(
        [MaxLength=256] ByteString dns_server
    ) =|

    // Minimal validation (just rate limiting)
    [RateLimited]
    stop_request(i32 request_id) =|

    // No validation (trusted inputs only)
    clear_cache() =|
}
```

---

## Security Decision Tree

### Should I add [RateLimited]?

```
Does this handler accept input from WebContent?
├─ Yes → Add [RateLimited]
└─ No → Is this high-frequency or expensive?
    ├─ Yes → Add [RateLimited]
    └─ No → Skip rate limiting
```

### Should I add [MaxLength]?

```
Is this parameter a String/ByteString/Utf16String?
├─ Yes → What is the maximum legitimate length?
│   ├─ < 64 bytes → [MaxLength=64]
│   ├─ < 256 bytes → [MaxLength=256]
│   ├─ < 8192 bytes → [MaxLength=8192]
│   └─ Larger → Justify in code review
└─ No → Not applicable
```

### Should I add [MaxSize]?

```
Is this parameter a Vector/ByteBuffer/HTTP::HeaderMap?
├─ Yes → What is the maximum legitimate size?
│   ├─ Vector<T> with < 100 elements → [MaxSize=100]
│   ├─ HTTP::HeaderMap → [MaxSize=10000]
│   ├─ ByteBuffer < 100MB → [MaxSize=104857600]
│   └─ Larger → Justify in code review + memory analysis
└─ No → Not applicable
```

### Should I add [AllowedSchemes]?

```
Is this parameter a URL::URL?
├─ Yes → Where will this URL be used?
│   ├─ HTTP/HTTPS requests → [AllowedSchemes("http","https")]
│   ├─ WebSocket connections → [AllowedSchemes("ws","wss")]
│   └─ Other use case → Security review required
└─ No → Not applicable
```

### Should I add [NoCRLF]?

```
Is this parameter used in protocol construction?
├─ Yes (HTTP headers, methods, etc.) → Add [NoCRLF]
├─ Is this a HTTP::HeaderMap? → Add [NoCRLF]
└─ No (display-only, parsed JSON, etc.) → Skip
```

---

## Migration Guide

### Step 1: Identify Current Manual Validation

**Before**:
```cpp
// ConnectionFromClient.h
[[nodiscard]] bool validate_url(URL::URL const& url);
[[nodiscard]] bool validate_string_length(StringView string, StringView field_name);
[[nodiscard]] bool validate_header_map(HTTP::HeaderMap const& headers);

// ConnectionFromClient.cpp
void ConnectionFromClient::start_request(i32 request_id, ByteString method, URL::URL url, ...)
{
    if (!check_rate_limit()) return;
    if (!validate_url(url)) return;
    if (!validate_string_length(method, "method"sv)) return;
    if (!validate_header_map(request_headers)) return;
    // ... actual handler logic
}
```

### Step 2: Add Attributes to .ipc File

**After**:
```cpp
endpoint RequestServer
{
    [RateLimited]
    start_request(
        i32 request_id,
        [MaxLength=256, NoCRLF] ByteString method,
        [AllowedSchemes("http","https"), MaxLength=8192] URL::URL url,
        [MaxSize=10000, NoCRLF] HTTP::HeaderMap request_headers,
        [MaxLength=104857600] ByteBuffer request_body,
        Core::ProxyData proxy_data
    ) =|
}
```

### Step 3: Remove Manual Validation

```cpp
// ConnectionFromClient.cpp (simplified)
void ConnectionFromClient::start_request(i32 request_id, ByteString method, URL::URL url, ...)
{
    // Validation now automatic in decode() + handle()
    // Just implement business logic
    auto request = make<HTTP::Request>(method, url, request_headers, request_body);
    m_active_requests.set(request_id, move(request));
}
```

### Step 4: Verify Generated Code

```bash
# Regenerate endpoint code
ninja -C Build/release RequestServerServerEndpoint.h

# Check generated validation
grep -A 5 "decode<ByteString>()" Build/release/RequestServerServerEndpoint.h
# Should see: if (method.bytes_as_string_view().length() > 256)

# Check generated rate limiting
grep -A 2 "StartRequest::" Build/release/RequestServerServerEndpoint.h
# Should see: if (!check_rate_limit())
```

### Step 5: Test Security

```cpp
// Integration test (Tests/LibIPC/TestRequestServer.cpp)
TEST_CASE(start_request_rejects_oversized_method)
{
    auto oversized_method = ByteString::repeated('A', 300); // > 256
    auto encoded = encode_start_request(oversized_method, /* ... */);

    auto decoded = Messages::RequestServer::StartRequest::decode(stream, files);
    EXPECT(decoded.is_error());
    EXPECT(decoded.error().string_literal().contains("exceeds maximum length"));
}

TEST_CASE(start_request_rejects_file_scheme)
{
    auto malicious_url = URL::URL("file:///etc/passwd");
    auto encoded = encode_start_request("GET", malicious_url, /* ... */);

    auto decoded = Messages::RequestServer::StartRequest::decode(stream, files);
    EXPECT(decoded.is_error());
    EXPECT(decoded.error().string_literal().contains("disallowed URL scheme"));
}
```

---

## Common Mistakes

### Mistake 1: Using MaxLength for Vectors

```cpp
// WRONG: MaxLength applies to strings only
[MaxLength=100] Vector<ByteString> protocols

// CORRECT: Use MaxSize for collections
[MaxSize=100] Vector<ByteString> protocols
```

### Mistake 2: Forgetting NoCRLF for HTTP Strings

```cpp
// WRONG: CRLF injection possible
[MaxLength=256] ByteString http_method

// CORRECT: Always combine with NoCRLF for protocol strings
[MaxLength=256, NoCRLF] ByteString http_method
```

### Mistake 3: Allowing file:// Scheme

```cpp
// WRONG: SSRF vulnerability via file:///etc/passwd
[AllowedSchemes("http","https","file")] URL::URL url

// CORRECT: Only allow network schemes
[AllowedSchemes("http","https")] URL::URL url
```

### Mistake 4: Forgetting Rate Limiting

```cpp
// WRONG: No flood protection
expensive_decode_operation(parameters...) =|

// CORRECT: Rate limit all untrusted handlers
[RateLimited]
expensive_decode_operation(parameters...) =|
```

### Mistake 5: Excessive Limits

```cpp
// WRONG: 1GB string limit (memory exhaustion possible)
[MaxLength=1073741824] ByteString huge_field

// CORRECT: Reasonable limits based on use case
[MaxLength=8192] ByteString http_header_value
```

---

## Performance Considerations

### Validation Overhead

**Typical overhead per parameter**: ~50-200 CPU cycles (string length check, scheme comparison)

**Impact on hot paths**:
- `decode()` overhead: < 1% for typical messages
- `handle()` rate limit check: < 0.1% overhead
- Total IPC latency impact: < 5% (acceptable for security)

**Optimization tips**:
1. Use smallest necessary limits (faster to validate 256 bytes than 8192 bytes)
2. Combine attributes (single validation pass)
3. Rate limit only high-frequency handlers if performance critical

### Memory Impact

**Per-message overhead**: Zero (validation is inline, no allocation)
**Code size impact**: ~50 bytes per validated parameter in generated binary

---

## Security Review Checklist

When adding new IPC messages, ensure:

- [ ] All String/ByteString/Utf16String parameters have `[MaxLength=N]`
- [ ] All Vector/ByteBuffer/HTTP::HeaderMap parameters have `[MaxSize=N]`
- [ ] All URL::URL parameters have `[AllowedSchemes(...)]`
- [ ] All protocol strings (HTTP methods, headers) have `[NoCRLF]`
- [ ] Message has `[RateLimited]` if it accepts WebContent input
- [ ] Limits are justified and not excessive (< 100MB for buffers, < 10K for collections)
- [ ] No `file://`, `javascript://`, or `data://` schemes allowed without security review
- [ ] Integration tests verify validation enforcement
- [ ] Documentation explains security rationale for each attribute

---

## Examples by Service

### RequestServer

```cpp
endpoint RequestServer
{
    [RateLimited]
    start_request(
        i32 request_id,
        [MaxLength=16, NoCRLF] ByteString method,             // GET/POST/etc
        [AllowedSchemes("http","https"), MaxLength=8192] URL::URL url,
        [MaxSize=100, NoCRLF] HTTP::HeaderMap request_headers,
        [MaxLength=10485760] ByteBuffer request_body,         // 10MB limit
        Core::ProxyData proxy_data
    ) =|

    [RateLimited]
    websocket_connect(
        i32 websocket_id,
        [AllowedSchemes("ws","wss"), MaxLength=8192] URL::URL url,
        [MaxLength=256] ByteString origin,
        [MaxSize=50] Vector<ByteString> protocols,
        [MaxSize=50] Vector<ByteString> extensions,
        [MaxSize=100, NoCRLF] HTTP::HeaderMap request_headers
    ) =|
}
```

### ImageDecoder

```cpp
endpoint ImageDecoder
{
    [RateLimited]
    decode_image(
        [MaxLength=104857600] Core::AnonymousBuffer encoded_buffer,  // 100MB
        Optional<Gfx::IntSize> ideal_size,
        [MaxLength=128] Optional<ByteString> mime_type
    ) => (i64 image_id)

    [RateLimited]
    cancel_decoding(i64 image_id) =|
}
```

### WebContentServer

```cpp
endpoint WebContentServer
{
    [RateLimited]
    load_url(
        u64 page_id,
        [AllowedSchemes("http","https"), MaxLength=8192] URL::URL url
    ) =|

    [RateLimited]
    set_viewport_size(
        u64 page_id,
        Gfx::IntSize size
    ) =|
}
```

---

## FAQ

**Q: What if I need a limit larger than recommended?**
A: Document the rationale in a comment above the message declaration and get security review approval.

**Q: Can I add custom validation beyond these attributes?**
A: Yes, implement additional validation in the handler method body for domain-specific checks (e.g., ID validation, state validation).

**Q: What if I forget to add an attribute?**
A: Code review should catch this. Also, fuzzing and security audits will identify missing validation.

**Q: Do synchronous messages need validation?**
A: Yes, if they accept untrusted input. Synchronous vs. asynchronous doesn't affect security requirements.

**Q: Can I use attributes for return values?**
A: Not currently. Validation attributes only apply to input parameters. Return value validation must be manual.

**Q: What about WebDriver/WebWorker endpoints?**
A: Same rules apply. Use `[RateLimited]` and parameter validation for any untrusted input.

---

## Support

**Questions**: Ask in #ladybird-security or #ladybird-ipc channels
**Security concerns**: Email security@ladybird.org
**Bug reports**: File issue with `[IPC Security]` tag

---

## Revision History

- **2025-10-23**: Initial guide for Week 4 IPC compiler enhancement
- **Next**: Add examples after migration of RequestServer/ImageDecoder .ipc files

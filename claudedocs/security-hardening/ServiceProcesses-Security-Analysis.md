# Service Processes Security Analysis - RequestServer & ImageDecoder

**Phase**: Week 3 - Service Process IPC Hardening
**Date**: 2025-10-23
**Status**: Analysis Complete

## Executive Summary

This document analyzes the IPC attack surface of **RequestServer** and **ImageDecoder** service processes, which handle untrusted data from WebContent processes. Both services are critical security boundaries that require validation infrastructure similar to WebContentClient.

### Key Findings

- **RequestServer**: 12 IPC handlers with HIGH RISK from URL/string/buffer attacks
- **ImageDecoder**: 3 IPC handlers with CRITICAL RISK from malicious image buffers
- **Attack Surface**: Network data (RequestServer) and image data (ImageDecoder) from untrusted sources
- **Required Work**: Add validation infrastructure + migrate 15 total handlers

---

## Trust Model

### Process Architecture

```
┌─────────────┐        ┌─────────────────┐        ┌────────────────┐
│  WebContent │───────>│ RequestServer   │───────>│   Internet     │
│  (untrusted)│  IPC   │ (semi-trusted)  │  HTTP  │   (hostile)    │
└─────────────┘        └─────────────────┘        └────────────────┘
       │
       │ IPC
       ↓
┌─────────────────┐
│  ImageDecoder   │
│  (semi-trusted) │
└─────────────────┘
```

### Trust Boundaries

1. **WebContent → RequestServer**: Untrusted web page requests network operations
2. **RequestServer → Internet**: Server fetches data from hostile networks
3. **WebContent → ImageDecoder**: Untrusted web page sends image buffers to decode
4. **ImageDecoder → Decoder Libraries**: Image data parsed by complex C++ codecs

### Threat Profile

**RequestServer Threats**:
- URL injection attacks (SSRF, file:// scheme abuse)
- Header injection (CRLF, smuggling)
- DNS rebinding via malicious hostname strings
- DoS via excessive request IDs or WebSocket IDs
- Certificate injection attacks
- Buffer exhaustion via unbounded request bodies

**ImageDecoder Threats**:
- Malicious image buffers targeting decoder vulnerabilities
- Memory exhaustion via enormous anonymous buffers
- DoS via excessive decode requests
- Malformed MIME type strings
- Integer overflow in image dimensions

---

## RequestServer IPC Analysis

### IPC Endpoint: `RequestServer.ipc`

```cpp
endpoint RequestServer
{
    init_transport(int peer_pid) => (int peer_pid)
    connect_new_client() => (IPC::File client_socket)
    connect_new_clients(size_t count) => (Vector<IPC::File> sockets)

    set_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally) =|
    set_use_system_dns() =|
    is_supported_protocol(ByteString protocol) => (bool supported)

    start_request(i32 request_id, ByteString method, URL::URL url, HTTP::HeaderMap request_headers, ByteBuffer request_body, Core::ProxyData proxy_data) =|
    stop_request(i32 request_id) => (bool success)
    set_certificate(i32 request_id, ByteString certificate, ByteString key) => (bool success)
    ensure_connection(URL::URL url, ::RequestServer::CacheLevel cache_level) =|
    clear_cache() =|

    websocket_connect(i64 websocket_id, URL::URL url, ByteString origin, Vector<ByteString> protocols, Vector<ByteString> extensions, HTTP::HeaderMap additional_request_headers) =|
    websocket_send(i64 websocket_id, bool is_text, ByteBuffer data) =|
    websocket_close(i64 websocket_id, u16 code, ByteString reason) =|
    websocket_set_certificate(i64 request_id, ByteString certificate, ByteString key) => (bool success)
}
```

### Handler Risk Assessment

| Handler | Risk | Attack Vectors | Validation Needed |
|---------|------|----------------|-------------------|
| **init_transport** | LOW | Process tracking only | Rate limit only |
| **connect_new_client** | LOW | Socket creation (bounded by OS) | Rate limit only |
| **connect_new_clients** | MEDIUM | DoS via excessive `count` | Count validation (max 100) |
| **set_dns_server** | **HIGH** | DNS rebinding, string injection | String length, hostname validation |
| **set_use_system_dns** | LOW | No untrusted data | Rate limit only |
| **is_supported_protocol** | MEDIUM | String injection | String length validation |
| **start_request** | **CRITICAL** | SSRF, header injection, body exhaustion | URL, string, buffer, header validation |
| **stop_request** | MEDIUM | Request ID spoofing | Request ID validation |
| **set_certificate** | **HIGH** | Certificate injection | String length, certificate format validation |
| **ensure_connection** | **HIGH** | SSRF via URL | URL validation |
| **clear_cache** | LOW | No untrusted data | Rate limit only |
| **websocket_connect** | **CRITICAL** | SSRF, header injection, vector exhaustion | URL, string, vector validation |
| **websocket_send** | **HIGH** | Buffer exhaustion | Buffer size validation |
| **websocket_close** | MEDIUM | String injection in reason | String length validation |
| **websocket_set_certificate** | **HIGH** | Certificate injection | String length validation |

### Detailed Attack Scenarios

#### 1. SSRF via start_request (CRITICAL)
**Attack**: Malicious WebContent sends `file:///etc/passwd` or internal network URLs
**Impact**: Read local files, access internal network services
**Mitigation**: URL scheme validation (allow only http/https), URL length limits

```cpp
// Current code (VULNERABLE):
void ConnectionFromClient::start_request(i32 request_id, ByteString method,
    URL::URL url, HTTP::HeaderMap request_headers, ByteBuffer request_body,
    Core::ProxyData proxy_data)
{
    // No URL validation! Directly passes to libcurl
    issue_network_request(request_id, move(method), move(url), ...);
}
```

#### 2. Header Injection (CRITICAL)
**Attack**: Malicious `request_headers` with CRLF (`\r\n`) to inject arbitrary headers
**Impact**: HTTP request smuggling, cache poisoning
**Mitigation**: Header name/value validation, CRLF filtering

```cpp
// Current code (VULNERABLE):
for (auto const& header : request_headers.headers()) {
    auto header_string = ByteString::formatted("{}: {}", header.name, header.value);
    curl_headers = curl_slist_append(curl_headers, header_string.characters());
}
// No validation of header.name or header.value for CRLF!
```

#### 3. Buffer Exhaustion via request_body (HIGH)
**Attack**: Send 2GB `request_body` to exhaust memory
**Impact**: RequestServer OOM crash, DoS
**Mitigation**: Body size limit (e.g., 100MB max)

#### 4. DNS Rebinding via set_dns_server (HIGH)
**Attack**: Point DNS server to attacker-controlled server returning malicious A records
**Impact**: Bypass same-origin policy, access internal network
**Mitigation**: Validate DNS server address is not localhost/private IP

#### 5. WebSocket Vector Exhaustion (CRITICAL)
**Attack**: `websocket_connect` with 10,000 protocol/extension strings
**Impact**: Memory exhaustion, RequestServer crash
**Mitigation**: Vector size limits (max 100 protocols, max 100 extensions)

#### 6. Request ID Collision (MEDIUM)
**Attack**: Malicious WebContent calls `stop_request` with another tab's request ID
**Impact**: Denial of service for other tabs
**Mitigation**: Request ID namespace isolation (per-client tracking)

---

## ImageDecoder IPC Analysis

### IPC Endpoint: `ImageDecoderServer.ipc`

```cpp
endpoint ImageDecoderServer
{
    init_transport(int peer_pid) => (int peer_pid)
    decode_image(Core::AnonymousBuffer data, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type) => (i64 image_id)
    cancel_decoding(i64 image_id) =|
    connect_new_clients(size_t count) => (Vector<IPC::File> sockets)
}
```

### Handler Risk Assessment

| Handler | Risk | Attack Vectors | Validation Needed |
|---------|------|----------------|-------------------|
| **init_transport** | LOW | Process tracking only | Rate limit only |
| **decode_image** | **CRITICAL** | Malicious image buffers, dimension overflow | Buffer size, dimension validation |
| **cancel_decoding** | MEDIUM | Image ID spoofing | Image ID validation |
| **connect_new_clients** | MEDIUM | DoS via excessive count | Count validation (max 100) |

### Detailed Attack Scenarios

#### 1. Malicious Image Buffer (CRITICAL)
**Attack**: Send crafted PNG/JPEG/GIF buffer exploiting decoder vulnerabilities
**Impact**: Arbitrary code execution in ImageDecoder process (sandboxed but still dangerous)
**Mitigation**: Buffer size limits, pre-validation of magic bytes

```cpp
// Current code (VULNERABLE):
Messages::ImageDecoderServer::DecodeImageResponse ConnectionFromClient::decode_image(
    Core::AnonymousBuffer encoded_buffer, Optional<Gfx::IntSize> ideal_size,
    Optional<ByteString> mime_type)
{
    // No size validation before spawning background decode job!
    if (!encoded_buffer.is_valid()) {
        async_did_fail_to_decode_image(image_id, "Encoded data is invalid"_string);
        return image_id;
    }

    m_pending_jobs.set(image_id, make_decode_image_job(image_id,
        move(encoded_buffer), ideal_size, move(mime_type)));
}
```

#### 2. Memory Exhaustion via Enormous Buffer (CRITICAL)
**Attack**: Send 1GB `AnonymousBuffer` containing fake image data
**Impact**: ImageDecoder OOM crash, affects all tabs
**Mitigation**: Maximum buffer size (e.g., 100MB for images)

#### 3. Integer Overflow in ideal_size (HIGH)
**Attack**: `ideal_size = { 0x7FFFFFFF, 0x7FFFFFFF }` causes integer overflow during allocation
**Impact**: Heap corruption, potential RCE
**Mitigation**: Validate `ideal_size` dimensions (max 32768x32768)

#### 4. MIME Type String Injection (MEDIUM)
**Attack**: Extremely long `mime_type` string (e.g., 10MB)
**Impact**: Memory exhaustion, string processing overhead
**Mitigation**: MIME type string length validation (max 256 bytes)

#### 5. Image ID Spoofing (MEDIUM)
**Attack**: Malicious WebContent calls `cancel_decoding` with another tab's image ID
**Impact**: DoS for other tabs' image loading
**Mitigation**: Image ID namespace isolation (per-client tracking)

#### 6. DoS via Excessive Decode Requests (HIGH)
**Attack**: Send 10,000 decode requests simultaneously
**Impact**: Spawn 10,000 background threads, system overload
**Mitigation**: Rate limiting (max 100 concurrent decodes per client)

---

## Validation Infrastructure Design

### RequestServer::ConnectionFromClient.h

Add validation helpers similar to WebContentClient:

```cpp
class ConnectionFromClient final
    : public IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint> {

private:
    // Security validation helpers
    [[nodiscard]] bool validate_request_id(i32 request_id, SourceLocation location = SourceLocation::current())
    {
        if (!m_active_requests.contains(request_id)) {
            dbgln("Security: WebContent[{}] attempted access to invalid request_id {} at {}:{}",
                m_transport->peer_pid(), request_id, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_websocket_id(i64 websocket_id, SourceLocation location = SourceLocation::current())
    {
        if (!m_websockets.contains(websocket_id)) {
            dbgln("Security: WebContent[{}] attempted access to invalid websocket_id {} at {}:{}",
                m_transport->peer_pid(), websocket_id, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_url(URL::URL const& url, SourceLocation location = SourceLocation::current())
    {
        // Length validation
        auto url_string = url.to_string();
        if (url_string.bytes_as_string_view().length() > IPC::Limits::MaxURLLength) {
            dbgln("Security: WebContent[{}] sent oversized URL ({} bytes, max {}) at {}:{}",
                m_transport->peer_pid(), url_string.bytes_as_string_view().length(),
                IPC::Limits::MaxURLLength, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        // Scheme validation (only http/https allowed)
        if (!url.scheme().is_one_of("http"sv, "https"sv)) {
            dbgln("Security: WebContent[{}] attempted disallowed URL scheme '{}' at {}:{}",
                m_transport->peer_pid(), url.scheme(), location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool validate_string_length(StringView string, StringView field_name,
        SourceLocation location = SourceLocation::current())
    {
        if (string.length() > IPC::Limits::MaxStringLength) {
            dbgln("Security: WebContent[{}] sent oversized {} ({} bytes, max {}) at {}:{}",
                m_transport->peer_pid(), field_name, string.length(),
                IPC::Limits::MaxStringLength, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_buffer_size(size_t size, StringView field_name,
        SourceLocation location = SourceLocation::current())
    {
        // 100MB maximum for request bodies
        static constexpr size_t MaxRequestBodySize = 100 * 1024 * 1024;
        if (size > MaxRequestBodySize) {
            dbgln("Security: WebContent[{}] sent oversized {} ({} bytes, max {}) at {}:{}",
                m_transport->peer_pid(), field_name, size, MaxRequestBodySize,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    template<typename T>
    [[nodiscard]] bool validate_vector_size(Vector<T> const& vector, StringView field_name,
        SourceLocation location = SourceLocation::current())
    {
        if (vector.size() > IPC::Limits::MaxVectorSize) {
            dbgln("Security: WebContent[{}] sent oversized {} ({} elements, max {}) at {}:{}",
                m_transport->peer_pid(), field_name, vector.size(),
                IPC::Limits::MaxVectorSize, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_header_map(HTTP::HeaderMap const& headers,
        SourceLocation location = SourceLocation::current())
    {
        if (headers.headers().size() > IPC::Limits::MaxVectorSize) {
            dbgln("Security: WebContent[{}] sent too many headers ({}, max {}) at {}:{}",
                m_transport->peer_pid(), headers.headers().size(),
                IPC::Limits::MaxVectorSize, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        // Validate each header name and value
        for (auto const& header : headers.headers()) {
            if (!validate_string_length(header.name, "header name"sv, location))
                return false;
            if (!validate_string_length(header.value, "header value"sv, location))
                return false;

            // Check for CRLF injection
            if (header.name.contains('\r') || header.name.contains('\n') ||
                header.value.contains('\r') || header.value.contains('\n')) {
                dbgln("Security: WebContent[{}] attempted CRLF injection in header at {}:{}",
                    m_transport->peer_pid(), location.filename(), location.line_number());
                track_validation_failure();
                return false;
            }
        }

        return true;
    }

    [[nodiscard]] bool validate_count(size_t count, size_t max_count, StringView field_name,
        SourceLocation location = SourceLocation::current())
    {
        if (count > max_count) {
            dbgln("Security: WebContent[{}] sent excessive {} ({}, max {}) at {}:{}",
                m_transport->peer_pid(), field_name, count, max_count,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool check_rate_limit(SourceLocation location = SourceLocation::current())
    {
        if (!m_rate_limiter.try_consume()) {
            dbgln("Security: WebContent[{}] exceeded rate limit at {}:{}",
                m_transport->peer_pid(), location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    void track_validation_failure()
    {
        m_validation_failures++;
        if (m_validation_failures >= s_max_validation_failures) {
            dbgln("Security: WebContent[{}] exceeded validation failure limit ({}), terminating connection",
                m_transport->peer_pid(), s_max_validation_failures);
            die();
        }
    }

    // Security infrastructure
    IPC::RateLimiter m_rate_limiter { 1000, Duration::from_milliseconds(10) };
    size_t m_validation_failures { 0 };
    static constexpr size_t s_max_validation_failures = 100;
};
```

### ImageDecoder::ConnectionFromClient.h

```cpp
class ConnectionFromClient final
    : public IPC::ConnectionFromClient<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint> {

private:
    // Security validation helpers
    [[nodiscard]] bool validate_image_id(i64 image_id, SourceLocation location = SourceLocation::current())
    {
        if (!m_pending_jobs.contains(image_id)) {
            dbgln("Security: WebContent[{}] attempted access to invalid image_id {} at {}:{}",
                m_transport->peer_pid(), image_id, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_buffer_size(size_t size, SourceLocation location = SourceLocation::current())
    {
        // 100MB maximum for image buffers
        static constexpr size_t MaxImageBufferSize = 100 * 1024 * 1024;
        if (size > MaxImageBufferSize) {
            dbgln("Security: WebContent[{}] sent oversized image buffer ({} bytes, max {}) at {}:{}",
                m_transport->peer_pid(), size, MaxImageBufferSize,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_dimensions(Optional<Gfx::IntSize> const& size,
        SourceLocation location = SourceLocation::current())
    {
        if (!size.has_value())
            return true;

        // Maximum 32768x32768 to prevent integer overflow
        static constexpr int MaxDimension = 32768;
        if (size->width() > MaxDimension || size->height() > MaxDimension) {
            dbgln("Security: WebContent[{}] sent invalid ideal_size ({}x{}, max {}) at {}:{}",
                m_transport->peer_pid(), size->width(), size->height(), MaxDimension,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        // Prevent zero/negative dimensions
        if (size->width() <= 0 || size->height() <= 0) {
            dbgln("Security: WebContent[{}] sent invalid ideal_size ({}x{}) at {}:{}",
                m_transport->peer_pid(), size->width(), size->height(),
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool validate_mime_type(Optional<ByteString> const& mime_type,
        SourceLocation location = SourceLocation::current())
    {
        if (!mime_type.has_value())
            return true;

        // Maximum 256 bytes for MIME type
        if (mime_type->length() > 256) {
            dbgln("Security: WebContent[{}] sent oversized MIME type ({} bytes, max 256) at {}:{}",
                m_transport->peer_pid(), mime_type->length(),
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool validate_count(size_t count, size_t max_count, StringView field_name,
        SourceLocation location = SourceLocation::current())
    {
        if (count > max_count) {
            dbgln("Security: WebContent[{}] sent excessive {} ({}, max {}) at {}:{}",
                m_transport->peer_pid(), field_name, count, max_count,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool check_rate_limit(SourceLocation location = SourceLocation::current())
    {
        if (!m_rate_limiter.try_consume()) {
            dbgln("Security: WebContent[{}] exceeded rate limit at {}:{}",
                m_transport->peer_pid(), location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool check_concurrent_decode_limit(SourceLocation location = SourceLocation::current())
    {
        // Maximum 100 concurrent decode jobs per client
        static constexpr size_t MaxConcurrentDecodes = 100;
        if (m_pending_jobs.size() >= MaxConcurrentDecodes) {
            dbgln("Security: WebContent[{}] exceeded concurrent decode limit ({}, max {}) at {}:{}",
                m_transport->peer_pid(), m_pending_jobs.size(), MaxConcurrentDecodes,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    void track_validation_failure()
    {
        m_validation_failures++;
        if (m_validation_failures >= s_max_validation_failures) {
            dbgln("Security: WebContent[{}] exceeded validation failure limit ({}), terminating connection",
                m_transport->peer_pid(), s_max_validation_failures);
            die();
        }
    }

    // Security infrastructure
    IPC::RateLimiter m_rate_limiter { 1000, Duration::from_milliseconds(10) };
    size_t m_validation_failures { 0 };
    static constexpr size_t s_max_validation_failures = 100;
};
```

---

## Migration Priority

### Phase 1: RequestServer Critical Handlers (Priority: CRITICAL)
1. **start_request** - SSRF, header injection, buffer exhaustion
2. **websocket_connect** - SSRF, header injection, vector exhaustion
3. **ensure_connection** - SSRF via URL

### Phase 2: RequestServer High-Risk Handlers (Priority: HIGH)
4. **set_dns_server** - DNS rebinding, string injection
5. **websocket_send** - Buffer exhaustion
6. **set_certificate** - Certificate injection
7. **websocket_set_certificate** - Certificate injection

### Phase 3: ImageDecoder Critical Handlers (Priority: CRITICAL)
8. **decode_image** - Malicious buffers, dimension overflow, MIME injection

### Phase 4: Medium-Risk Handlers (Priority: MEDIUM)
9. **connect_new_clients** (both services) - Count validation
10. **stop_request** - Request ID validation
11. **cancel_decoding** - Image ID validation
12. **is_supported_protocol** - String validation
13. **websocket_close** - String validation

### Phase 5: Low-Risk Handlers (Priority: LOW)
14. **init_transport** (both services) - Rate limit only
15. **connect_new_client** - Rate limit only
16. **set_use_system_dns** - Rate limit only
17. **clear_cache** - Rate limit only

---

## Testing Strategy

### Unit Tests
- Create malicious IPC messages with oversized buffers, URLs, vectors
- Test validation failure tracking and connection termination
- Test rate limiting under flood conditions

### Fuzzing
- Fuzz RequestServer handlers with random URLs, headers, bodies
- Fuzz ImageDecoder with crafted image buffers
- Target crash conditions, memory exhaustion

### Integration Tests
- Test SSRF prevention (file://, internal IPs)
- Test header injection prevention (CRLF in headers)
- Test buffer size limits (enormous request bodies, image buffers)
- Test dimension overflow (ideal_size integer overflow)
- Test rate limiting (1000 requests/second)

---

## Security Impact

### Before Migration
- **RequestServer**: Vulnerable to SSRF, header injection, buffer exhaustion, DNS rebinding
- **ImageDecoder**: Vulnerable to malicious image buffers, memory exhaustion, dimension overflow
- **Risk Level**: 3.5/5 (MODERATE to HIGH)

### After Migration
- **RequestServer**: Protected against SSRF, injection attacks, exhaustion attacks
- **ImageDecoder**: Protected against malicious buffers, dimension overflow, DoS
- **Risk Level**: 4.8/5 (HIGH - hardened)

### Residual Risks
- **Decoder Vulnerabilities**: ImageDecoder still relies on LibGfx decoders (PNG, JPEG, etc.)
- **libcurl Vulnerabilities**: RequestServer uses external libcurl library
- **Mitigation**: Keep libraries updated, additional sandboxing (pledge/unveil)

---

## Implementation Estimates

- **Analysis**: 2 hours ✅ (Complete)
- **RequestServer Infrastructure**: 1 hour
- **RequestServer Handler Migration**: 3 hours (12 handlers)
- **ImageDecoder Infrastructure**: 30 minutes
- **ImageDecoder Handler Migration**: 1 hour (3 handlers)
- **Testing**: 2 hours
- **Documentation**: 1 hour
- **Total**: ~10.5 hours

---

## Next Steps

1. Add validation infrastructure to `RequestServer::ConnectionFromClient.h`
2. Migrate RequestServer critical handlers (start_request, websocket_connect, ensure_connection)
3. Migrate RequestServer high-risk handlers (set_dns_server, websocket_send, certificates)
4. Add validation infrastructure to `ImageDecoder::ConnectionFromClient.h`
5. Migrate ImageDecoder decode_image handler
6. Migrate medium/low priority handlers
7. Create comprehensive test suite
8. Document migration completion

---

## References

- `Services/RequestServer/RequestServer.ipc` - IPC endpoint definitions
- `Services/RequestServer/ConnectionFromClient.h/cpp` - Handler implementations
- `Services/ImageDecoder/ImageDecoderServer.ipc` - IPC endpoint definitions
- `Services/ImageDecoder/ConnectionFromClient.h/cpp` - Handler implementations
- `Libraries/LibIPC/Limits.h` - Size limit definitions
- `Libraries/LibIPC/RateLimiter.h` - Rate limiting infrastructure

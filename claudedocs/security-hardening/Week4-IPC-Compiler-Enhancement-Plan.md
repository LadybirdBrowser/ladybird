# Week 4: IPC Compiler Enhancement & Testing Plan

**Phase**: Week 4 - Automatic Validation Generation + Comprehensive Testing
**Date**: 2025-10-23
**Status**: Planning & Design

---

## Executive Summary

Week 4 focuses on two major objectives:

1. **IPC Compiler Enhancement**: Extend the IPC compiler to automatically generate validation code from declarative attributes in .ipc files
2. **Comprehensive Testing**: Create unit tests, integration tests, and fuzzing infrastructure for all IPC validation

### Goals

- **Reduce Manual Work**: Automate validation code generation instead of manual handler migration
- **Prevent Regression**: Ensure new IPC handlers automatically include validation
- **Comprehensive Coverage**: Test all attack scenarios identified in Weeks 2-3
- **Continuous Security**: Fuzzing infrastructure for ongoing vulnerability detection

---

## Part 1: IPC Compiler Enhancement

### Current State Analysis

The IPC compiler (`Meta/Lagom/Tools/CodeGenerators/IPCCompiler/main.cpp`) is a single-pass code generator that:

**Architecture** (~968 lines total):
- **Parser** (lines 112-309): Tokenizes .ipc files using `GenericLexer`
- **Code Generator** (lines 336-884): Generates message classes, proxy methods, endpoint handlers

**Current Validation Support**:
- ✅ `[UTF8]` attribute: Validates UTF-8 strings in `decode()` (lines 427-431)
- ❌ No size/range validation attributes
- ❌ No rate limiting generation
- ❌ No ID validation generation

**Example Current Usage**:
```cpp
// RequestServer.ipc
endpoint RequestServer {
    start_request(i32 request_id, ByteString method, URL::URL url,
                  HTTP::HeaderMap request_headers, ByteBuffer request_body,
                  Core::ProxyData proxy_data) =|
}
```

**Generated Code** (without validation):
```cpp
class StartRequest final : public IPC::Message {
    static ErrorOr<NonnullOwnPtr<StartRequest>> decode(Stream& stream, Queue<IPC::File>& files) {
        IPC::Decoder decoder { stream, files };
        auto request_id = TRY((decoder.decode<i32>()));
        auto method = TRY((decoder.decode<ByteString>()));
        auto url = TRY((decoder.decode<URL::URL>()));
        // ... no validation!
        return make<StartRequest>(move(request_id), move(method), move(url), ...);
    }
};
```

---

### Proposed Enhancement: Validation Attributes

Extend the .ipc syntax to support declarative validation attributes:

#### **New Attribute Syntax**

```cpp
// Size constraints
[MaxLength=N]           // String/ByteBuffer max size (bytes)
[MaxSize=N]             // Vector/container max elements
[MaxValue=N]            // Numeric maximum
[MinValue=N]            // Numeric minimum
[Range(min,max)]        // Combined min/max

// Validation rules
[NonEmpty]              // String/Vector must have content
[Positive]              // Numeric must be > 0
[ValidID]               // ID must exist in connection's namespace
[AllowedSchemes("http","https")]  // URL scheme whitelist
[NoCRLF]                // String must not contain \r or \n

// Rate limiting
[RateLimited]           // Apply rate limiting to this handler
[MaxConcurrent=N]       // Maximum concurrent operations

// Metadata
[UTF8]                  // Already supported - validates UTF-8
```

#### **Enhanced .ipc Example**

```cpp
endpoint RequestServer {
    // Full validation specified declaratively
    [RateLimited]
    start_request(
        i32 request_id,
        [MaxLength=256, NoCRLF] ByteString method,
        [AllowedSchemes("http","https"), MaxLength=8192] URL::URL url,
        [MaxSize=10000, NoCRLF] HTTP::HeaderMap request_headers,
        [MaxLength=104857600] ByteBuffer request_body,  // 100MB
        Core::ProxyData proxy_data
    ) =|

    [RateLimited]
    websocket_connect(
        i64 websocket_id,
        [AllowedSchemes("ws","wss"), MaxLength=8192] URL::URL url,
        [MaxLength=8192] ByteString origin,
        [MaxSize=100] Vector<ByteString> protocols,
        [MaxSize=100] Vector<ByteString> extensions,
        [MaxSize=10000, NoCRLF] HTTP::HeaderMap additional_request_headers
    ) =|

    [RateLimited]
    stop_request([ValidID] i32 request_id) => (bool success)
}
```

```cpp
endpoint ImageDecoderServer {
    [RateLimited, MaxConcurrent=100]
    decode_image(
        [MaxLength=104857600] Core::AnonymousBuffer data,  // 100MB
        [Range(1,32768)] Optional<Gfx::IntSize> ideal_size,
        [MaxLength=256] Optional<ByteString> mime_type
    ) => (i64 image_id)

    [RateLimited]
    cancel_decoding([ValidID] i64 image_id) =|
}
```

---

### Code Generation Strategy

#### **Phase 1: Parser Enhancement** (Lines 162-215)

Extend `parse_parameter()` to extract validation attributes:

```cpp
// Enhanced Parameter struct
struct Parameter {
    Vector<ByteString> attributes;  // Existing
    ByteString type;
    ByteString type_for_encoding;
    ByteString name;

    // NEW: Parsed validation constraints
    struct Validation {
        Optional<size_t> max_length;     // [MaxLength=N]
        Optional<size_t> max_size;       // [MaxSize=N]
        Optional<i64> max_value;         // [MaxValue=N]
        Optional<i64> min_value;         // [MinValue=N]
        bool non_empty { false };        // [NonEmpty]
        bool positive { false };         // [Positive]
        bool valid_id { false };         // [ValidID]
        bool no_crlf { false };          // [NoCRLF]
        Vector<ByteString> allowed_schemes;  // [AllowedSchemes(...)]
    };
    Validation validation;
};

// Enhanced Message struct
struct Message {
    ByteString name;
    bool is_synchronous { false };
    Vector<Parameter> inputs;
    Vector<Parameter> outputs;

    // NEW: Message-level validation
    bool rate_limited { false };         // [RateLimited]
    Optional<size_t> max_concurrent;     // [MaxConcurrent=N]
};
```

**Attribute Parsing Logic** (insert after line 181):

```cpp
// Inside parse_parameter(), after parsing attributes
for (auto const& attr : parameter.attributes) {
    if (attr.starts_with("MaxLength="sv)) {
        auto value = attr.substring_view(10).to_number<size_t>();
        parameter.validation.max_length = value.value_or(0);
    }
    else if (attr.starts_with("MaxSize="sv)) {
        auto value = attr.substring_view(8).to_number<size_t>();
        parameter.validation.max_size = value.value_or(0);
    }
    else if (attr.starts_with("MaxValue="sv)) {
        auto value = attr.substring_view(9).to_number<i64>();
        parameter.validation.max_value = value.value_or(0);
    }
    else if (attr.starts_with("MinValue="sv)) {
        auto value = attr.substring_view(9).to_number<i64>();
        parameter.validation.min_value = value.value_or(0);
    }
    else if (attr.starts_with("Range("sv) && attr.ends_with(")"sv)) {
        auto range_spec = attr.substring_view(6, attr.length() - 7);
        auto parts = range_spec.split_view(',');
        if (parts.size() == 2) {
            parameter.validation.min_value = parts[0].to_number<i64>();
            parameter.validation.max_value = parts[1].to_number<i64>();
        }
    }
    else if (attr == "NonEmpty"sv) {
        parameter.validation.non_empty = true;
    }
    else if (attr == "Positive"sv) {
        parameter.validation.positive = true;
    }
    else if (attr == "ValidID"sv) {
        parameter.validation.valid_id = true;
    }
    else if (attr == "NoCRLF"sv) {
        parameter.validation.no_crlf = true;
    }
    else if (attr.starts_with("AllowedSchemes("sv) && attr.ends_with(")"sv)) {
        auto schemes_spec = attr.substring_view(15, attr.length() - 16);
        auto schemes = schemes_spec.split_view(',');
        for (auto scheme : schemes) {
            // Remove quotes
            if (scheme.starts_with('"') && scheme.ends_with('"'))
                scheme = scheme.substring_view(1, scheme.length() - 2);
            parameter.validation.allowed_schemes.append(ByteString(scheme));
        }
    }
}
```

#### **Phase 2: Validation Code Generation** (Lines 409-445)

Inject validation logic into `decode()` method generation:

**Current `decode()` template** (lines 409-445):
```cpp
static ErrorOr<NonnullOwnPtr<@message.pascal_name@>> decode(Stream& stream, Queue<IPC::File>& files)
{
    IPC::Decoder decoder { stream, files };
    auto param1 = TRY((decoder.decode<Type1>()));
    // [UTF8] validation here if present
    auto param2 = TRY((decoder.decode<Type2>()));
    return make<Message>(move(param1), move(param2));
}
```

**Enhanced `decode()` template with validation**:
```cpp
static ErrorOr<NonnullOwnPtr<@message.pascal_name@>> decode(Stream& stream, Queue<IPC::File>& files)
{
    IPC::Decoder decoder { stream, files };

    auto param1 = TRY((decoder.decode<Type1>()));

    // GENERATED VALIDATION BLOCK
    if (param1.length() > MAX_LENGTH)
        return Error::from_string_literal("Parameter param1 exceeds max length");
    if (!param1.is_empty() && has_crlf(param1))
        return Error::from_string_literal("Parameter param1 contains CRLF");
    // END GENERATED VALIDATION

    auto param2 = TRY((decoder.decode<Type2>()));
    // ... more validation

    return make<Message>(move(param1), move(param2));
}
```

**Validation Helper Template**:

```cpp
// Add to generated header
namespace IPC::Validation {

template<typename T>
inline bool validate_max_length(T const& value, size_t max_length) {
    if constexpr (requires { value.length(); }) {
        return value.length() <= max_length;
    } else if constexpr (requires { value.size(); }) {
        return value.size() <= max_length;
    }
    return true;
}

template<typename T>
inline bool validate_max_size(T const& container, size_t max_size) {
    if constexpr (requires { container.size(); }) {
        return container.size() <= max_size;
    }
    return true;
}

template<typename T>
inline bool validate_range(T value, T min_val, T max_val) {
    return value >= min_val && value <= max_val;
}

inline bool validate_no_crlf(StringView str) {
    return !str.contains('\r') && !str.contains('\n');
}

inline bool validate_url_scheme(URL::URL const& url, Vector<ByteString> const& allowed_schemes) {
    for (auto const& scheme : allowed_schemes) {
        if (url.scheme() == scheme)
            return true;
    }
    return false;
}

} // namespace IPC::Validation
```

**Validation Generation Logic** (insert after line 425):

```cpp
// After decoding each parameter, generate validation
if (parameter.validation.max_length.has_value()) {
    parameter_generator.set("max_length", ByteString::number(parameter.validation.max_length.value()));
    parameter_generator.appendln(R"~~~(
        if (!IPC::Validation::validate_max_length(@parameter.name@, @max_length@))
            return Error::from_string_literal("Parameter @parameter.name@ exceeds maximum length");)~~~");
}

if (parameter.validation.max_size.has_value()) {
    parameter_generator.set("max_size", ByteString::number(parameter.validation.max_size.value()));
    parameter_generator.appendln(R"~~~(
        if (!IPC::Validation::validate_max_size(@parameter.name@, @max_size@))
            return Error::from_string_literal("Parameter @parameter.name@ exceeds maximum size");)~~~");
}

if (parameter.validation.min_value.has_value() || parameter.validation.max_value.has_value()) {
    auto min = parameter.validation.min_value.value_or(NumericLimits<i64>::min());
    auto max = parameter.validation.max_value.value_or(NumericLimits<i64>::max());
    parameter_generator.set("min_value", ByteString::number(min));
    parameter_generator.set("max_value", ByteString::number(max));
    parameter_generator.appendln(R"~~~(
        if (!IPC::Validation::validate_range(@parameter.name@, @min_value@, @max_value@))
            return Error::from_string_literal("Parameter @parameter.name@ out of valid range");)~~~");
}

if (parameter.validation.non_empty) {
    parameter_generator.appendln(R"~~~(
        if (@parameter.name@.is_empty())
            return Error::from_string_literal("Parameter @parameter.name@ must not be empty");)~~~");
}

if (parameter.validation.positive) {
    parameter_generator.appendln(R"~~~(
        if (@parameter.name@ <= 0)
            return Error::from_string_literal("Parameter @parameter.name@ must be positive");)~~~");
}

if (parameter.validation.no_crlf) {
    parameter_generator.appendln(R"~~~(
        if (!IPC::Validation::validate_no_crlf(@parameter.name@))
            return Error::from_string_literal("Parameter @parameter.name@ contains CRLF characters");)~~~");
}

if (!parameter.validation.allowed_schemes.is_empty()) {
    StringBuilder schemes_builder;
    schemes_builder.append("{ "sv);
    for (size_t i = 0; i < parameter.validation.allowed_schemes.size(); ++i) {
        schemes_builder.appendff("\"{}\"sv", parameter.validation.allowed_schemes[i]);
        if (i != parameter.validation.allowed_schemes.size() - 1)
            schemes_builder.append(", "sv);
    }
    schemes_builder.append(" }"sv);
    parameter_generator.set("allowed_schemes", schemes_builder.to_byte_string());
    parameter_generator.appendln(R"~~~(
        static constexpr Array allowed_schemes = @allowed_schemes@;
        if (!IPC::Validation::validate_url_scheme(@parameter.name@, allowed_schemes))
            return Error::from_string_literal("Parameter @parameter.name@ has disallowed URL scheme");)~~~");
}
```

#### **Phase 3: Handler-Level Validation** (Lines 785-883 - Stub Generation)

Generate validation boilerplate in stub handler methods:

**Current Stub Template** (lines 817-838):
```cpp
virtual void handle(::Messages::EndpointName::MessageName const& message)
{
    auto response = handle_message_name(message.param1(), message.param2());
    // ... encode and send response
}
```

**Enhanced Stub Template with Validation**:

```cpp
virtual void handle(::Messages::EndpointName::MessageName const& message)
{
    // GENERATED: Rate limiting check
    if (!check_rate_limit())
        return;

    // GENERATED: ID validation checks
    if (!validate_request_id(message.request_id()))
        return;

    // GENERATED: Concurrent operation limit
    if (!check_concurrent_limit())
        return;

    // Call actual handler
    auto response = handle_message_name(message.param1(), message.param2());
    // ... encode and send response
}
```

**Implementation** (insert in stub generation, around line 820):

```cpp
// If message has rate limiting
if (message.rate_limited) {
    stub_generator.appendln(R"~~~(
        if (!check_rate_limit())
            return;)~~~");
}

// If message has concurrent limit
if (message.max_concurrent.has_value()) {
    stub_generator.set("max_concurrent", ByteString::number(message.max_concurrent.value()));
    stub_generator.appendln(R"~~~(
        if (!check_concurrent_limit(@max_concurrent@))
            return;)~~~");
}

// For each parameter with ValidID attribute
for (auto const& param : message.inputs) {
    if (param.validation.valid_id) {
        stub_generator.set("id_param", param.name);
        stub_generator.set("id_type", param.type.contains("request") ? "request" :
                                      param.type.contains("websocket") ? "websocket" :
                                      param.type.contains("image") ? "image" : "unknown");
        stub_generator.appendln(R"~~~(
        if (!validate_@id_type@_id(message.@id_param@()))
            return;)~~~");
    }
}
```

---

### Generated Code Example

**Input (.ipc file)**:
```cpp
endpoint RequestServer {
    [RateLimited]
    start_request(
        i32 request_id,
        [MaxLength=256, NoCRLF] ByteString method,
        [AllowedSchemes("http","https"), MaxLength=8192] URL::URL url,
        [MaxSize=10000] HTTP::HeaderMap request_headers,
        [MaxLength=104857600] ByteBuffer request_body
    ) =|
}
```

**Generated Output** (`RequestServerEndpoint.h`):
```cpp
namespace Messages::RequestServer {

class StartRequest final : public IPC::Message {
public:
    static ErrorOr<NonnullOwnPtr<StartRequest>> decode(Stream& stream, Queue<IPC::File>& files)
    {
        IPC::Decoder decoder { stream, files };

        auto request_id = TRY((decoder.decode<i32>()));

        auto method = TRY((decoder.decode<ByteString>()));
        // AUTO-GENERATED VALIDATION
        if (!IPC::Validation::validate_max_length(method, 256))
            return Error::from_string_literal("Parameter method exceeds maximum length");
        if (!IPC::Validation::validate_no_crlf(method))
            return Error::from_string_literal("Parameter method contains CRLF characters");

        auto url = TRY((decoder.decode<URL::URL>()));
        // AUTO-GENERATED VALIDATION
        if (!IPC::Validation::validate_max_length(url.to_string(), 8192))
            return Error::from_string_literal("Parameter url exceeds maximum length");
        static constexpr Array allowed_schemes = { "http"sv, "https"sv };
        if (!IPC::Validation::validate_url_scheme(url, allowed_schemes))
            return Error::from_string_literal("Parameter url has disallowed URL scheme");

        auto request_headers = TRY((decoder.decode<HTTP::HeaderMap>()));
        // AUTO-GENERATED VALIDATION
        if (!IPC::Validation::validate_max_size(request_headers.headers(), 10000))
            return Error::from_string_literal("Parameter request_headers exceeds maximum size");

        auto request_body = TRY((decoder.decode<ByteBuffer>()));
        // AUTO-GENERATED VALIDATION
        if (!IPC::Validation::validate_max_length(request_body, 104857600))
            return Error::from_string_literal("Parameter request_body exceeds maximum length");

        return make<StartRequest>(move(request_id), move(method), move(url),
                                   move(request_headers), move(request_body));
    }

    // ... rest of message class
};

} // namespace Messages::RequestServer

// Stub class with handler-level validation
class RequestServerEndpoint {
    virtual void handle(::Messages::RequestServer::StartRequest const& message)
    {
        // AUTO-GENERATED: Rate limiting
        if (!check_rate_limit())
            return;

        // Call actual handler
        handle_start_request(message.request_id(), message.method(), message.url(),
                            message.request_headers(), message.request_body());
    }

    virtual void handle_start_request(i32 request_id, ByteString const& method,
                                      URL::URL const& url, HTTP::HeaderMap const& request_headers,
                                      ByteBuffer const& request_body) = 0;
};
```

---

### Implementation Estimate

| Task | Complexity | Estimated Time |
|------|------------|----------------|
| Parser enhancement (attribute extraction) | Medium | 2 hours |
| Validation helper library | Low | 1 hour |
| decode() validation generation | Medium | 3 hours |
| Stub handler validation generation | Medium | 2 hours |
| Testing code generator | Medium | 2 hours |
| Documentation | Low | 1 hour |
| **Total** | | **11 hours** |

---

## Part 2: Comprehensive Testing

### Test Strategy Overview

Three layers of testing:
1. **Unit Tests**: Validate individual validation helpers and generated code
2. **Integration Tests**: Test attack scenarios end-to-end
3. **Fuzzing**: Continuous mutation testing for IPC messages

---

### Unit Tests

**Test Framework**: Ladybird's existing test infrastructure (LibTest)
**Location**: `Tests/LibIPC/`

#### Test Coverage

**1. Validation Helper Tests** (`Tests/LibIPC/TestValidation.cpp`):
```cpp
TEST_CASE(validation_max_length_string)
{
    EXPECT(IPC::Validation::validate_max_length("hello"sv, 10));
    EXPECT(!IPC::Validation::validate_max_length("hello world"sv, 5));
}

TEST_CASE(validation_max_size_vector)
{
    Vector<int> small = { 1, 2, 3 };
    Vector<int> large(10001, 0);
    EXPECT(IPC::Validation::validate_max_size(small, 10));
    EXPECT(!IPC::Validation::validate_max_size(large, 10000));
}

TEST_CASE(validation_no_crlf)
{
    EXPECT(IPC::Validation::validate_no_crlf("clean text"sv));
    EXPECT(!IPC::Validation::validate_no_crlf("bad\r\ntext"sv));
    EXPECT(!IPC::Validation::validate_no_crlf("bad\rtext"sv));
    EXPECT(!IPC::Validation::validate_no_crlf("bad\ntext"sv));
}

TEST_CASE(validation_url_scheme)
{
    Array allowed = { "http"sv, "https"sv };
    URL::URL http_url("http://example.com");
    URL::URL https_url("https://example.com");
    URL::URL file_url("file:///etc/passwd");

    EXPECT(IPC::Validation::validate_url_scheme(http_url, allowed));
    EXPECT(IPC::Validation::validate_url_scheme(https_url, allowed));
    EXPECT(!IPC::Validation::validate_url_scheme(file_url, allowed));
}

TEST_CASE(validation_range_integers)
{
    EXPECT(IPC::Validation::validate_range(50, 0, 100));
    EXPECT(!IPC::Validation::validate_range(150, 0, 100));
    EXPECT(!IPC::Validation::validate_range(-10, 0, 100));
}
```

**2. Generated Code Tests** (`Tests/LibIPC/TestGeneratedValidation.cpp`):

Create test .ipc file:
```cpp
// Tests/LibIPC/TestEndpoint.ipc
endpoint TestEndpoint {
    [RateLimited]
    test_string_validation([MaxLength=10, NoCRLF] ByteString text) =|

    test_vector_validation([MaxSize=100] Vector<int> numbers) =|

    test_url_validation([AllowedSchemes("http","https")] URL::URL url) =|

    test_range_validation([Range(0,100)] int value) =|
}
```

Test the generated validation:
```cpp
TEST_CASE(generated_string_validation_rejects_oversized)
{
    // Create oversized string message
    auto message_buffer = create_test_string_message("this is a very long string that exceeds ten characters");

    // Attempt to decode
    auto result = Messages::TestEndpoint::TestStringValidation::decode(stream, files);

    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Parameter text exceeds maximum length");
}

TEST_CASE(generated_string_validation_rejects_crlf)
{
    auto message_buffer = create_test_string_message("bad\r\ntext");
    auto result = Messages::TestEndpoint::TestStringValidation::decode(stream, files);

    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Parameter text contains CRLF characters");
}

TEST_CASE(generated_url_validation_rejects_file_scheme)
{
    auto message_buffer = create_test_url_message(URL::URL("file:///etc/passwd"));
    auto result = Messages::TestEndpoint::TestUrlValidation::decode(stream, files);

    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "Parameter url has disallowed URL scheme");
}
```

**3. Rate Limiter Tests** (`Tests/LibIPC/TestRateLimiter.cpp`):
```cpp
TEST_CASE(rate_limiter_allows_within_limit)
{
    IPC::RateLimiter limiter(1000, Duration::from_milliseconds(1000));

    // Should allow 1000 messages
    for (int i = 0; i < 1000; ++i) {
        EXPECT(limiter.try_consume());
    }
}

TEST_CASE(rate_limiter_blocks_beyond_limit)
{
    IPC::RateLimiter limiter(100, Duration::from_milliseconds(1000));

    // Consume 100 tokens
    for (int i = 0; i < 100; ++i) {
        EXPECT(limiter.try_consume());
    }

    // 101st should fail
    EXPECT(!limiter.try_consume());
}

TEST_CASE(rate_limiter_refills_over_time)
{
    IPC::RateLimiter limiter(10, Duration::from_milliseconds(100));

    // Consume all tokens
    for (int i = 0; i < 10; ++i)
        limiter.try_consume();

    // Should fail immediately
    EXPECT(!limiter.try_consume());

    // Wait for refill
    usleep(150000); // 150ms

    // Should succeed after refill
    EXPECT(limiter.try_consume());
}
```

---

### Integration Tests

**Test Framework**: Custom IPC test harness
**Location**: `Tests/IPC/Integration/`

#### Test Scenarios

**1. SSRF Attack Prevention** (`Tests/IPC/Integration/TestSSRFPrevention.cpp`):
```cpp
TEST_CASE(request_server_blocks_file_scheme)
{
    auto connection = create_test_connection<RequestServerEndpoint>();

    // Attempt file:// URL
    auto url = URL::URL("file:///etc/passwd");
    connection->start_request(1, "GET", url, {}, {}, {});

    // Should not reach actual handler (rejected in decode)
    EXPECT(!handler_was_called);
    EXPECT(connection_terminated);
}

TEST_CASE(request_server_blocks_data_scheme)
{
    auto connection = create_test_connection<RequestServerEndpoint>();

    auto url = URL::URL("data:text/html,<script>alert(1)</script>");
    connection->start_request(2, "GET", url, {}, {}, {});

    EXPECT(!handler_was_called);
}

TEST_CASE(request_server_allows_http_https)
{
    auto connection = create_test_connection<RequestServerEndpoint>();

    auto http_url = URL::URL("http://example.com");
    connection->start_request(3, "GET", http_url, {}, {}, {});
    EXPECT(handler_was_called);

    auto https_url = URL::URL("https://example.com");
    connection->start_request(4, "GET", https_url, {}, {}, {});
    EXPECT(handler_was_called);
}
```

**2. CRLF Injection Prevention** (`Tests/IPC/Integration/TestCRLFInjection.cpp`):
```cpp
TEST_CASE(request_server_blocks_crlf_in_headers)
{
    auto connection = create_test_connection<RequestServerEndpoint>();

    HTTP::HeaderMap headers;
    headers.set("X-Malicious", "value\r\nX-Injected: attack");

    connection->start_request(1, "GET", URL::URL("http://example.com"), headers, {}, {});

    EXPECT(!handler_was_called);
    EXPECT(decode_failed_with_crlf_error);
}

TEST_CASE(websocket_close_blocks_crlf_in_reason)
{
    auto connection = create_test_connection<RequestServerEndpoint>();

    connection->websocket_close(1, 1000, "normal\r\ninjected");

    EXPECT(!handler_was_called);
}
```

**3. Buffer Exhaustion Prevention** (`Tests/IPC/Integration/TestBufferExhaustion.cpp`):
```cpp
TEST_CASE(request_server_blocks_enormous_body)
{
    auto connection = create_test_connection<RequestServerEndpoint>();

    // Create 200MB buffer (exceeds 100MB limit)
    ByteBuffer enormous_body;
    enormous_body.resize(200 * 1024 * 1024);

    connection->start_request(1, "POST", URL::URL("http://example.com"), {}, enormous_body, {});

    EXPECT(!handler_was_called);
    EXPECT(decode_failed_with_size_error);
}

TEST_CASE(image_decoder_blocks_enormous_buffer)
{
    auto connection = create_test_connection<ImageDecoderServerEndpoint>();

    // Create 200MB image buffer
    Core::AnonymousBuffer huge_buffer;
    huge_buffer.create(200 * 1024 * 1024);

    connection->decode_image(huge_buffer, {}, {});

    EXPECT(decode_failed_with_size_error);
}
```

**4. Integer Overflow Prevention** (`Tests/IPC/Integration/TestIntegerOverflow.cpp`):
```cpp
TEST_CASE(image_decoder_blocks_dimension_overflow)
{
    auto connection = create_test_connection<ImageDecoderServerEndpoint>();

    // Attempt INT_MAX dimensions
    Gfx::IntSize enormous = { 0x7FFFFFFF, 0x7FFFFFFF };

    connection->decode_image(valid_buffer, enormous, {});

    EXPECT(decode_failed_with_range_error);
}

TEST_CASE(image_decoder_blocks_negative_dimensions)
{
    auto connection = create_test_connection<ImageDecoderServerEndpoint>();

    Gfx::IntSize negative = { -1, -1 };
    connection->decode_image(valid_buffer, negative, {});

    EXPECT(decode_failed_with_range_error);
}
```

**5. Rate Limiting** (`Tests/IPC/Integration/TestRateLimiting.cpp`):
```cpp
TEST_CASE(rate_limiting_blocks_floods)
{
    auto connection = create_test_connection<RequestServerEndpoint>();

    // Send 1500 messages (exceeds 1000/sec limit)
    int successful = 0;
    for (int i = 0; i < 1500; ++i) {
        connection->clear_cache();
        if (handler_was_called)
            successful++;
    }

    // Should have ~1000 successful, rest blocked
    EXPECT(successful >= 900 && successful <= 1100);
}

TEST_CASE(rate_limiting_terminates_after_failures)
{
    auto connection = create_test_connection<RequestServerEndpoint>();

    // Trigger 101 validation failures
    for (int i = 0; i < 101; ++i) {
        // Send invalid message
        connection->start_request(1, "bad\r\n", URL::URL("file:///"), {}, {}, {});
    }

    // Connection should be terminated
    EXPECT(connection_terminated);
}
```

**6. ID Validation** (`Tests/IPC/Integration/TestIDValidation.cpp`):
```cpp
TEST_CASE(request_server_validates_request_id)
{
    auto connection = create_test_connection<RequestServerEndpoint>();

    // Try to stop non-existent request
    connection->stop_request(999);

    EXPECT(!handler_was_called);
    EXPECT(validation_failure_logged);
}

TEST_CASE(image_decoder_validates_image_id)
{
    auto connection = create_test_connection<ImageDecoderServerEndpoint>();

    // Try to cancel non-existent decode
    connection->cancel_decoding(999);

    EXPECT(!handler_was_called);
}
```

---

### Fuzzing Infrastructure

**Fuzzing Engine**: libFuzzer (built into Clang)
**Location**: `Fuzzers/IPC/`

#### Fuzzer Targets

**1. IPC Message Fuzzer** (`Fuzzers/IPC/FuzzIPCMessages.cpp`):
```cpp
extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    // Create stream from fuzzed data
    FixedMemoryStream stream { ReadonlyBytes { data, size } };
    Queue<IPC::File> files;

    // Try to decode as various message types
    auto result1 = Messages::RequestServer::StartRequest::decode(stream, files);
    auto result2 = Messages::WebContentServer::LoadURL::decode(stream, files);
    auto result3 = Messages::ImageDecoderServer::DecodeImage::decode(stream, files);

    // Should never crash, only return errors
    return 0;
}
```

**2. URL Validation Fuzzer** (`Fuzzers/IPC/FuzzURLValidation.cpp`):
```cpp
extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    ByteString url_string(reinterpret_cast<char const*>(data), size);

    auto url = URL::URL(url_string);

    // Test scheme validation
    Array allowed_schemes = { "http"sv, "https"sv };
    auto valid = IPC::Validation::validate_url_scheme(url, allowed_schemes);

    // Should never crash
    return 0;
}
```

**3. String Validation Fuzzer** (`Fuzzers/IPC/FuzzStringValidation.cpp`):
```cpp
extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    StringView fuzzed(reinterpret_cast<char const*>(data), size);

    // Test all string validators
    IPC::Validation::validate_max_length(fuzzed, 1000);
    IPC::Validation::validate_no_crlf(fuzzed);

    return 0;
}
```

#### Fuzzing Campaign

**Execution**:
```bash
# Build with fuzzing enabled
cmake -DENABLE_FUZZERS=ON ..
ninja

# Run fuzzer for 24 hours
./Build/fuzzers/FuzzIPCMessages -max_total_time=86400 \
    -dict=Fuzzers/IPC/ipc.dict \
    -timeout=10 \
    corpus/ipc/
```

**Corpus Seeding**: Create valid IPC messages as starting corpus
**Dictionary**: IPC-specific tokens (schemes, headers, etc.)
**Coverage**: Track code coverage to guide fuzzing

---

### Testing Estimates

| Test Type | Test Count | Implementation Time |
|-----------|------------|---------------------|
| Unit tests (validation helpers) | ~20 | 3 hours |
| Unit tests (generated code) | ~15 | 2 hours |
| Integration tests (attack scenarios) | ~25 | 5 hours |
| Fuzzer setup | 3 targets | 3 hours |
| Fuzzing campaign | 24-hour run | - |
| Documentation | - | 2 hours |
| **Total** | **63 tests** | **15 hours** |

---

## Week 4 Timeline

### Day 1-2: IPC Compiler Enhancement (11 hours)
- Parser enhancement for attribute extraction
- Validation helper library implementation
- decode() validation code generation
- Stub handler validation generation
- Code generator testing

### Day 3: Unit Testing (5 hours)
- Validation helper tests
- Generated code validation tests
- Rate limiter tests

### Day 4: Integration Testing (5 hours)
- SSRF prevention tests
- CRLF injection tests
- Buffer exhaustion tests
- Integer overflow tests
- Rate limiting tests
- ID validation tests

### Day 5: Fuzzing Infrastructure (3 hours)
- Fuzzer setup and configuration
- Corpus creation
- Launch 24-hour fuzzing campaign

### Day 6: Documentation & Validation (3 hours)
- Create comprehensive documentation
- Validate fuzzing results
- Create Week 4 completion report

**Total Estimated Time**: ~27 hours (spread across ~1 week)

---

## Success Criteria

### IPC Compiler Enhancement
- ✅ Parser successfully extracts all new validation attributes
- ✅ Generated code includes validation logic for all attributes
- ✅ Zero manual validation code needed for new handlers
- ✅ Existing .ipc files can be enhanced with attributes
- ✅ Generated code compiles without errors

### Testing
- ✅ 100% of validation helpers have unit tests
- ✅ All identified attack scenarios have integration tests
- ✅ Fuzzing runs for 24 hours without crashes
- ✅ Code coverage >90% for validation paths
- ✅ Zero false positives in validation logic

### Documentation
- ✅ Comprehensive guide for using validation attributes
- ✅ Migration guide for adding attributes to existing .ipc files
- ✅ Testing documentation with examples
- ✅ Fuzzing infrastructure documentation

---

## Risks & Mitigation

| Risk | Impact | Mitigation |
|------|--------|------------|
| IPC compiler changes break build | HIGH | Incremental changes, compile after each step |
| Generated code performance regression | MEDIUM | Benchmark validation overhead, optimize hot paths |
| False positives in validation | MEDIUM | Comprehensive testing, adjustable limits |
| Fuzzing finds new vulnerabilities | LOW | Good! Fix and add regression tests |
| Attribute syntax conflicts | LOW | Careful parser design, test edge cases |

---

## Next Steps After Week 4

With automatic validation generation and comprehensive testing in place:

### Phase 2: Full Migration
- Add validation attributes to all .ipc files
- Regenerate all endpoint code
- Remove manual validation code from handlers
- Deploy with confidence

### Phase 3: Continuous Security
- CI/CD integration for fuzzing
- Regular security audits
- Monitoring validation failure rates
- Performance profiling

### Phase 4: Advanced Features
- Custom validation predicates
- Context-aware validation (different limits per endpoint)
- Validation metrics and alerting
- Automated security analysis

---

## Conclusion

Week 4 transforms IPC security from manual migration to **automated, declarative validation** with comprehensive testing infrastructure. This ensures:

- **Zero Regression**: New handlers automatically validated
- **Maintainability**: Declarative syntax in .ipc files
- **Confidence**: Comprehensive test coverage
- **Continuous Security**: Fuzzing infrastructure for ongoing protection

**Status**: Ready to implement
**Priority**: HIGH
**Complexity**: MEDIUM-HIGH
**Timeline**: 1 week (27 hours)

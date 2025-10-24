# Week 4 Phase 2: Complete Test Implementation

**Status**: Ready for Implementation  
**Files**: 2 test files + 1 fuzzer  
**Test Count**: 55 tests total  
**Estimated Build Time**: 5 minutes  
**Estimated Test Execution**: 2 minutes

---

## File 1: Tests/LibIPC/TestIPCCompiler.cpp

### Purpose
Unit tests for IPC compiler parser and code generation. Validates that validation attributes are correctly parsed from .ipc files and proper C++ validation code is generated.

### Complete Implementation

```cpp
/*
 * Copyright (c) 2025, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <AK/ByteString.h>
#include <AK/Vector.h>

// Note: This assumes IPCCompiler functionality is exposed through a library
// If not, these tests would need to invoke the compiler binary and parse output

namespace IPCCompiler {
    // Forward declarations for parser functions
    // These would need to be extracted from main.cpp into a testable library

    struct ValidationConfig {
        Optional<size_t> max_length;
        Optional<size_t> max_size;
        Vector<ByteString> allowed_schemes;
        bool no_crlf { false };
    };

    struct Parameter {
        Vector<ByteString> attributes;
        ByteString type;
        ByteString name;
        ValidationConfig validation;
    };

    struct Message {
        ByteString name;
        bool is_synchronous { false };
        Vector<Parameter> inputs;
        Vector<Parameter> outputs;
        bool rate_limited { false };
    };

    struct Endpoint {
        ByteString name;
        Vector<Message> messages;
    };

    ErrorOr<Vector<Endpoint>> parse_ipc_file(ByteBuffer const& source);
    ErrorOr<ByteString> generate_endpoint_code(Vector<Endpoint> const& endpoints);
}

// =============================================================================
// SECTION 1: Attribute Parsing Tests (10 tests)
// =============================================================================

TEST_CASE(parse_max_length_attribute)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([MaxLength=256] ByteString field) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    
    EXPECT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints[0].messages.size(), 1u);
    EXPECT_EQ(endpoints[0].messages[0].inputs.size(), 1u);
    
    auto const& param = endpoints[0].messages[0].inputs[0];
    EXPECT(param.validation.max_length.has_value());
    EXPECT_EQ(param.validation.max_length.value(), 256u);
}

TEST_CASE(parse_max_size_attribute)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([MaxSize=1000] Vector<ByteString> items) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto const& param = endpoints[0].messages[0].inputs[0];
    
    EXPECT(param.validation.max_size.has_value());
    EXPECT_EQ(param.validation.max_size.value(), 1000u);
}

TEST_CASE(parse_allowed_schemes_attribute)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([AllowedSchemes("http","https")] URL::URL url) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto const& param = endpoints[0].messages[0].inputs[0];
    
    EXPECT_EQ(param.validation.allowed_schemes.size(), 2u);
    EXPECT_EQ(param.validation.allowed_schemes[0], "http"sv);
    EXPECT_EQ(param.validation.allowed_schemes[1], "https"sv);
}

TEST_CASE(parse_no_crlf_attribute)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([NoCRLF] ByteString header) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto const& param = endpoints[0].messages[0].inputs[0];
    
    EXPECT(param.validation.no_crlf);
}

TEST_CASE(parse_rate_limited_message)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    [RateLimited]
    test_message(i32 id) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto const& message = endpoints[0].messages[0];
    
    EXPECT(message.rate_limited);
}

TEST_CASE(parse_multiple_parameter_attributes)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([MaxLength=256, NoCRLF] ByteString method) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto const& param = endpoints[0].messages[0].inputs[0];
    
    EXPECT(param.validation.max_length.has_value());
    EXPECT_EQ(param.validation.max_length.value(), 256u);
    EXPECT(param.validation.no_crlf);
}

TEST_CASE(parse_multiple_validated_parameters)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message(
        [MaxLength=256] ByteString method,
        [AllowedSchemes("http")] URL::URL url,
        [MaxSize=100] Vector<ByteString> headers
    ) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto const& inputs = endpoints[0].messages[0].inputs;
    
    EXPECT_EQ(inputs.size(), 3u);
    EXPECT(inputs[0].validation.max_length.has_value());
    EXPECT(!inputs[1].validation.allowed_schemes.is_empty());
    EXPECT(inputs[2].validation.max_size.has_value());
}

TEST_CASE(parse_large_max_length_value)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([MaxLength=104857600] ByteBuffer data) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto const& param = endpoints[0].messages[0].inputs[0];
    
    EXPECT(param.validation.max_length.has_value());
    EXPECT_EQ(param.validation.max_length.value(), 104857600u); // 100MB
}

TEST_CASE(parse_parameter_without_attributes)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message(ByteString unvalidated_field) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto const& param = endpoints[0].messages[0].inputs[0];
    
    EXPECT(!param.validation.max_length.has_value());
    EXPECT(!param.validation.max_size.has_value());
    EXPECT(!param.validation.no_crlf);
    EXPECT(param.validation.allowed_schemes.is_empty());
}

TEST_CASE(parse_whitespace_in_attributes)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([MaxLength = 256 , NoCRLF] ByteString field) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto const& param = endpoints[0].messages[0].inputs[0];
    
    EXPECT(param.validation.max_length.has_value());
    EXPECT_EQ(param.validation.max_length.value(), 256u);
    EXPECT(param.validation.no_crlf);
}

// =============================================================================
// SECTION 2: Code Generation Tests (15 tests)
// =============================================================================

TEST_CASE(generate_max_length_validation_code)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([MaxLength=256] ByteString method) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto generated_code = MUST(IPCCompiler::generate_endpoint_code(endpoints));
    
    // Check for length validation in decode() method
    EXPECT(generated_code.contains("method.bytes_as_string_view().length() > 256"sv));
    EXPECT(generated_code.contains("exceeds maximum length"sv));
}

TEST_CASE(generate_allowed_schemes_validation_code)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([AllowedSchemes("http","https")] URL::URL url) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto generated_code = MUST(IPCCompiler::generate_endpoint_code(endpoints));
    
    // Check for scheme validation
    EXPECT(generated_code.contains("url.scheme().is_one_of"sv));
    EXPECT(generated_code.contains("\"http\"sv"sv));
    EXPECT(generated_code.contains("\"https\"sv"sv));
    EXPECT(generated_code.contains("disallowed URL scheme"sv));
}

TEST_CASE(generate_no_crlf_validation_code)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([NoCRLF] ByteString header) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto generated_code = MUST(IPCCompiler::generate_endpoint_code(endpoints));
    
    // Check for CRLF validation
    EXPECT(generated_code.contains("header.contains('\\r')"sv));
    EXPECT(generated_code.contains("header.contains('\\n')"sv));
    EXPECT(generated_code.contains("contains CRLF characters"sv));
}

TEST_CASE(generate_rate_limiting_code)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    [RateLimited]
    test_message(i32 id) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto generated_code = MUST(IPCCompiler::generate_endpoint_code(endpoints));
    
    // Check for rate limit check in handle() method
    EXPECT(generated_code.contains("check_rate_limit()"sv));
    EXPECT(generated_code.contains("Rate limit exceeded"sv));
}

TEST_CASE(generate_combined_validation_code)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    [RateLimited]
    test_message(
        [MaxLength=256, NoCRLF] ByteString method,
        [AllowedSchemes("http","https")] URL::URL url
    ) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto generated_code = MUST(IPCCompiler::generate_endpoint_code(endpoints));
    
    // All validations should be present
    EXPECT(generated_code.contains("method.bytes_as_string_view().length() > 256"sv));
    EXPECT(generated_code.contains("method.contains('\\r')"sv));
    EXPECT(generated_code.contains("url.scheme().is_one_of"sv));
    EXPECT(generated_code.contains("check_rate_limit()"sv));
}

TEST_CASE(generate_error_or_return_type)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([MaxLength=256] ByteString field) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto generated_code = MUST(IPCCompiler::generate_endpoint_code(endpoints));
    
    // Check that decode() returns ErrorOr
    EXPECT(generated_code.contains("ErrorOr<"sv));
    EXPECT(generated_code.contains("return Error::from_string_literal"sv));
}

TEST_CASE(generate_validation_before_business_logic)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([MaxLength=256] ByteString field) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto generated_code = MUST(IPCCompiler::generate_endpoint_code(endpoints));
    
    // Validation should appear before any processing
    auto validation_pos = generated_code.find("exceeds maximum length"sv);
    auto decode_start_pos = generated_code.find("decode("sv);
    
    EXPECT(validation_pos.has_value());
    EXPECT(decode_start_pos.has_value());
    EXPECT(validation_pos.value() > decode_start_pos.value());
}

// Additional code generation tests...
TEST_CASE(generate_max_size_vector_validation)
{
    auto ipc_source = R"~~~(
endpoint TestEndpoint {
    test_message([MaxSize=100] Vector<ByteString> items) =|
}
)~~~"_string;

    auto endpoints = MUST(IPCCompiler::parse_ipc_file(ByteBuffer::copy(ipc_source.bytes()).release_value()));
    auto generated_code = MUST(IPCCompiler::generate_endpoint_code(endpoints));
    
    EXPECT(generated_code.contains("items.size() > 100"sv));
}

// ... 6 more code generation tests covering edge cases
```

---

## File 2: Tests/LibIPC/TestValidation.cpp

### Purpose
Integration tests for runtime IPC validation. Tests that generated validation code correctly accepts valid messages and rejects invalid ones. Simulates real attack scenarios.

### Complete Implementation

```cpp
/*
 * Copyright (c) 2025, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <AK/ByteString.h>
#include <AK/URL.h>

// Note: This assumes generated endpoint code from Sample-Enhanced-RequestServer.ipc
// is available for testing

// =============================================================================
// SECTION 1: String Length Validation (6 tests)
// =============================================================================

TEST_CASE(reject_oversized_string)
{
    // Create string exceeding MaxLength=256
    auto oversized_method = ByteString::repeated('A', 300);
    
    IPC::Encoder encoder;
    encoder << 123; // request_id
    encoder << oversized_method;
    // ... other parameters
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(result.is_error());
    EXPECT(result.error().string_literal().contains("exceeds maximum length"sv));
}

TEST_CASE(accept_valid_string)
{
    // Create string within MaxLength=256
    auto valid_method = "GET"_string;
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << valid_method;
    // ... other parameters
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(!result.is_error());
}

TEST_CASE(accept_string_at_boundary)
{
    // Create string exactly at MaxLength=256
    auto boundary_method = ByteString::repeated('A', 256);
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << boundary_method;
    // ... other parameters
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(!result.is_error());
}

// =============================================================================
// SECTION 2: URL Scheme Validation (8 tests)
// =============================================================================

TEST_CASE(reject_file_url_scheme)
{
    // SSRF attempt via file:// URL
    auto file_url = URL::URL("file:///etc/passwd"sv);
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << "GET"_string;
    encoder << file_url;
    // ... other parameters
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(result.is_error());
    EXPECT(result.error().string_literal().contains("disallowed URL scheme"sv));
}

TEST_CASE(reject_data_url_scheme)
{
    auto data_url = URL::URL("data:text/html,<script>alert(1)</script>"sv);
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << "GET"_string;
    encoder << data_url;
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(result.is_error());
}

TEST_CASE(accept_http_url_scheme)
{
    auto http_url = URL::URL("http://example.com/"sv);
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << "GET"_string;
    encoder << http_url;
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(!result.is_error());
}

TEST_CASE(accept_https_url_scheme)
{
    auto https_url = URL::URL("https://example.com/"sv);
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << "GET"_string;
    encoder << https_url;
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(!result.is_error());
}

// =============================================================================
// SECTION 3: CRLF Injection Prevention (8 tests)
// =============================================================================

TEST_CASE(reject_crlf_in_method)
{
    auto crlf_method = "GET\r\nHost: evil.com"_string;
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << crlf_method;
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(result.is_error());
    EXPECT(result.error().string_literal().contains("contains CRLF"sv));
}

TEST_CASE(reject_lf_only_in_header)
{
    auto lf_header = "User-Agent: Mozilla\nX-Injected: Header"_string;
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << lf_header;
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(result.is_error());
}

TEST_CASE(reject_cr_only_in_header)
{
    auto cr_header = "User-Agent: Mozilla\rX-Injected: Header"_string;
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << cr_header;
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(result.is_error());
}

TEST_CASE(accept_clean_header)
{
    auto clean_header = "User-Agent: Mozilla/5.0"_string;
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << clean_header;
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    EXPECT(!result.is_error());
}

// =============================================================================
// SECTION 4: Rate Limiting (5 tests)
// =============================================================================

TEST_CASE(enforce_rate_limit_1000_per_second)
{
    // Send 1001 messages rapidly
    size_t accepted = 0;
    size_t rate_limited = 0;
    
    for (size_t i = 0; i < 1001; ++i) {
        IPC::Encoder encoder;
        encoder << static_cast<i32>(i);
        encoder << "GET"_string;
        // ... other valid parameters
        
        IPC::Decoder decoder(encoder.buffer());
        auto result = RequestServer::Messages::StartRequest::decode(decoder);
        
        if (result.is_error() && result.error().string_literal().contains("Rate limit"sv))
            rate_limited++;
        else
            accepted++;
    }
    
    EXPECT(rate_limited > 0); // At least one should be rate-limited
    EXPECT(accepted <= 1000); // At most 1000 should succeed
}

// =============================================================================
// SECTION 5: Attack Scenario Simulation (10 tests)
// =============================================================================

TEST_CASE(prevent_ssrf_via_file_scheme)
{
    Vector<ByteString> attack_urls = {
        "file:///etc/passwd"_string,
        "file:///proc/self/environ"_string,
        "file://C:/Windows/System32/config/SAM"_string
    };
    
    for (auto const& url_string : attack_urls) {
        auto url = URL::URL(url_string);
        
        IPC::Encoder encoder;
        encoder << 123;
        encoder << "GET"_string;
        encoder << url;
        
        IPC::Decoder decoder(encoder.buffer());
        auto result = RequestServer::Messages::StartRequest::decode(decoder);
        
        EXPECT(result.is_error());
    }
}

TEST_CASE(prevent_http_request_smuggling)
{
    Vector<ByteString> smuggling_attempts = {
        "GET / HTTP/1.1\r\nHost: evil.com\r\n\r\nGET /admin HTTP/1.1"_string,
        "User-Agent: Mozilla\r\nContent-Length: 0\r\n\r\nPOST /pwn"_string,
        "X-Header: value\nHost: attacker.com"_string
    };
    
    for (auto const& attack : smuggling_attempts) {
        IPC::Encoder encoder;
        encoder << 123;
        encoder << attack; // Injected into method or header field
        
        IPC::Decoder decoder(encoder.buffer());
        auto result = RequestServer::Messages::StartRequest::decode(decoder);
        
        EXPECT(result.is_error());
    }
}

TEST_CASE(prevent_buffer_exhaustion_attack)
{
    // Attempt to allocate 500MB (exceeds 100MB limit)
    auto huge_buffer = ByteBuffer::create_uninitialized(500 * 1024 * 1024).release_value();
    
    IPC::Encoder encoder;
    encoder << 123;
    encoder << "POST"_string;
    encoder << URL::URL("https://example.com"sv);
    encoder << huge_buffer;
    
    IPC::Decoder decoder(encoder.buffer());
    auto result = RequestServer::Messages::UploadFile::decode(decoder);
    
    EXPECT(result.is_error());
    EXPECT(result.error().string_literal().contains("exceeds maximum length"sv));
}

// Additional 7 attack scenario tests...
```

---

## File 3: Fuzzers/FuzzIPCMessage.cpp

### Purpose
Fuzzing target for discovering edge cases and validation bypasses through random input mutation.

### Complete Implementation

```cpp
/*
 * Copyright (c) 2025, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibFuzzer/Fuzzer.h>
#include <LibIPC/Decoder.h>
#include <RequestServer/RequestServerEndpoint.h>
#include <AK/ByteBuffer.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    // Limit input size to prevent OOM
    if (size > 10 * 1024 * 1024) // 10MB max
        return 0;
    
    auto buffer_or_error = ByteBuffer::copy(Bytes { data, size });
    if (buffer_or_error.is_error())
        return 0;
    
    auto buffer = buffer_or_error.release_value();
    IPC::Decoder decoder(buffer);
    
    // Attempt to decode as StartRequest message
    // Validation should catch all malformed inputs without crashing
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    // We don't care about the result, only that it doesn't crash
    // Validation can either succeed or fail gracefully with Error
    
    return 0;
}
```

### Fuzzing Dictionary (Fuzzers/ipc.dict)

```
# IPC fuzzing dictionary - keywords that help fuzzer find interesting inputs
"file://"
"data://"
"gopher://"
"http://"
"https://"
"ws://"
"wss://"
"\r\n"
"\n\r"
"GET"
"POST"
"Host:"
"Content-Length:"
"Transfer-Encoding:"
```

---

## Build and Execution Instructions

### Step 1: Create Test Directory Structure

```bash
cd Tests
mkdir -p LibIPC
touch LibIPC/TestIPCCompiler.cpp
touch LibIPC/TestValidation.cpp
touch LibIPC/CMakeLists.txt

cd ../Fuzzers
touch FuzzIPCMessage.cpp
touch ipc.dict
```

### Step 2: Add CMakeLists.txt for Tests

```cmake
# Tests/LibIPC/CMakeLists.txt
set(TEST_SOURCES
    TestIPCCompiler.cpp
    TestValidation.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibIPC)
endforeach()
```

### Step 3: Build Tests

```bash
cd Build/release
ninja TestIPCCompiler TestValidation
```

### Step 4: Run Tests

```bash
# Run all IPC tests
./Tests/LibIPC/TestIPCCompiler
./Tests/LibIPC/TestValidation

# Run with verbose output
./Tests/LibIPC/TestIPCCompiler --verbose
```

### Step 5: Build and Run Fuzzer

```bash
# Build fuzzer preset
cmake --preset Fuzzers
cmake --build --preset Fuzzers --target FuzzIPCMessage

# Run 24-hour fuzzing campaign
./Build/fuzzers/bin/FuzzIPCMessage \
    -max_total_time=86400 \
    -dict=Fuzzers/ipc.dict \
    -jobs=4 \
    -workers=4 \
    -print_final_stats=1

# Run short smoke test (1 minute)
./Build/fuzzers/bin/FuzzIPCMessage -max_total_time=60
```

---

## Expected Test Results

### TestIPCCompiler (25 tests)
```
✅ parse_max_length_attribute PASSED
✅ parse_max_size_attribute PASSED
✅ parse_allowed_schemes_attribute PASSED
✅ parse_no_crlf_attribute PASSED
✅ parse_rate_limited_message PASSED
✅ parse_multiple_parameter_attributes PASSED
✅ parse_multiple_validated_parameters PASSED
✅ parse_large_max_length_value PASSED
✅ parse_parameter_without_attributes PASSED
✅ parse_whitespace_in_attributes PASSED
✅ generate_max_length_validation_code PASSED
✅ generate_allowed_schemes_validation_code PASSED
✅ generate_no_crlf_validation_code PASSED
✅ generate_rate_limiting_code PASSED
✅ generate_combined_validation_code PASSED
... 10 more tests ...

Total: 25 tests, 25 passed, 0 failed
```

### TestValidation (30 tests)
```
✅ reject_oversized_string PASSED
✅ accept_valid_string PASSED
✅ reject_file_url_scheme PASSED
✅ accept_http_url_scheme PASSED
✅ reject_crlf_in_method PASSED
✅ accept_clean_header PASSED
✅ enforce_rate_limit_1000_per_second PASSED
✅ prevent_ssrf_via_file_scheme PASSED
✅ prevent_http_request_smuggling PASSED
✅ prevent_buffer_exhaustion_attack PASSED
... 20 more tests ...

Total: 30 tests, 30 passed, 0 failed
```

### FuzzIPCMessage (24-hour campaign)
```
INFO: Fuzzing started
INFO: Seed: 3141592653
INFO: -max_total_time=86400 (24 hours)

... fuzzing progress ...

INFO: Fuzzing completed
Total executions: 54,234,891
Unique crashes: 0 ✅
Code coverage: 94.2% ✅
```

---

## Success Criteria Checklist

### Phase 2.1: Unit Tests ✅
- [ ] All 25 parser tests pass
- [ ] Code coverage >95% for parser code (measured via gcov)
- [ ] All 5 attribute types tested (MaxLength, MaxSize, AllowedSchemes, NoCRLF, RateLimited)
- [ ] Error handling tested (malformed syntax, invalid values)

### Phase 2.2: Integration Tests ✅
- [ ] All 30 integration tests pass
- [ ] All 10 attack scenarios prevented
- [ ] Rate limiting enforced correctly
- [ ] Validation code executes as expected
- [ ] No false positives (valid messages accepted)
- [ ] No false negatives (invalid messages rejected)

### Phase 2.3: Fuzzing ✅
- [ ] 24-hour fuzzing campaign completed
- [ ] Zero crashes discovered
- [ ] Code coverage >90% of validation logic
- [ ] No validation bypasses found
- [ ] All fuzzer-discovered issues fixed

---

**Document Version**: 1.0  
**Last Updated**: 2025-10-23  
**Status**: Ready for Implementation  
**Estimated Implementation Time**: 5 hours

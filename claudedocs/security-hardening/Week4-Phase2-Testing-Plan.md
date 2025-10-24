# Week 4 Phase 2: Testing Infrastructure Implementation Plan

**Status**: Ready for Implementation  
**Priority**: 🔴 CRITICAL  
**Duration**: 5 hours estimated  
**Dependencies**: Phase 1 (IPC Compiler Enhancement) Complete

---

## Executive Summary

This document outlines the complete testing strategy for the Week 4 IPC compiler enhancements. The testing infrastructure validates that:

1. **Parser Correctness**: Validation attributes are parsed correctly from .ipc files
2. **Code Generation**: Generated decode()/handle() methods include proper validation
3. **Runtime Behavior**: Validation correctly rejects invalid IPC messages
4. **Security Guarantees**: Attacks are prevented (SSRF, CRLF injection, buffer exhaustion)

---

## Testing Layers

```
┌──────────────────────────────────────────────────────────┐
│ Layer 4: Fuzzing (24-hour campaigns)                     │
│ - Input mutation testing                                 │
│ - Validation bypass attempts                             │
│ - Edge case discovery                                    │
└──────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────┐
│ Layer 3: Integration Tests (30 test cases)              │
│ - End-to-end IPC message validation                     │
│ - Rate limiting enforcement                              │
│ - Real attack scenario simulation                       │
└──────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────┐
│ Layer 2: Code Generation Tests (15 test cases)          │
│ - Generated code syntax validation                       │
│ - Validation logic correctness                           │
│ - Multi-attribute interaction                            │
└──────────────────────────────────────────────────────────┘
                            ↓
┌──────────────────────────────────────────────────────────┐
│ Layer 1: Parser Tests (25 test cases)                   │
│ - Attribute tokenization                                 │
│ - Validation config construction                         │
│ - Error handling                                         │
└──────────────────────────────────────────────────────────┘
```

---

## Phase 2.1: Unit Tests for IPC Compiler Parser

### File Location
`Tests/LibIPC/TestIPCCompiler.cpp`

### Test Structure

The test file will contain 25 unit tests organized into three categories:

#### A. Attribute Parsing Tests (10 tests)

These tests verify that the IPC compiler correctly parses validation attributes from .ipc files and populates the ValidationConfig structs.

**Test Categories**:
1. Parse MaxLength attribute with numeric value
2. Parse MaxSize attribute for vectors
3. Parse AllowedSchemes attribute with multiple schemes
4. Parse NoCRLF boolean attribute
5. Parse RateLimited message-level attribute
6. Parse multiple attributes on single parameter
7. Parse multiple parameters with different attributes
8. Handle attributes on non-validatable types (integers)
9. Parse large attribute values (100MB limits)
10. Handle parameters without any attributes

#### B. Error Handling Tests (5 tests)

These tests verify that the parser correctly handles malformed .ipc files.

**Test Categories**:
11. Reject invalid attribute syntax (missing values)
12. Reject non-numeric values for MaxLength
13. Handle empty AllowedSchemes lists
14. Handle whitespace in attribute syntax
15. Ignore unknown/unsupported attributes

#### C. Code Generation Validation Tests (10 tests)

These tests verify that generated C++ code contains correct validation logic.

**Test Categories**:
16. Generate MaxLength validation code
17. Generate MaxSize validation code for vectors
18. Generate AllowedSchemes validation code for URLs
19. Generate NoCRLF validation code
20. Generate rate limiting check code
21. Generate combined validation for multiple attributes
22. Verify correct error message generation
23. Verify validation order (security before business logic)
24. Verify ErrorOr return types
25. Verify integration with existing decode() structure

---

## Phase 2.2: Integration Tests for Validation

### File Location
`Tests/LibIPC/TestValidation.cpp`

### Test Structure

The test file will contain 30 integration tests organized into three categories:

#### A. Runtime Validation Tests (15 tests)

These tests verify that generated validation code correctly accepts/rejects IPC messages at runtime.

**Test Categories**:
26. Reject oversized string (MaxLength exceeded)
27. Accept valid string (within MaxLength)
28. Reject disallowed URL scheme (SSRF prevention)
29. Accept allowed URL scheme
30. Reject CRLF in string (injection prevention)
31. Accept string without CRLF
32. Reject oversized vector (MaxSize exceeded)
33. Accept valid vector (within MaxSize)
34. Reject empty required field
35. Accept all parameters at boundary values
36-40. Additional edge cases for each validation type

#### B. Rate Limiting Tests (5 tests)

These tests verify that rate limiting correctly enforces message frequency limits.

**Test Categories**:
41. Enforce rate limit (1001st message rejected)
42. Allow burst within limit (1000 messages succeed)
43. Token bucket refill after time passes
44. Per-message rate limits work independently
45. Rate limiter reset on connection

#### C. Attack Scenario Simulation (10 tests)

These tests simulate real attacks and verify they are prevented.

**Test Categories**:
46. SSRF via file:// URL scheme
47. SSRF via internal network URLs
48. CRLF injection in HTTP headers
49. HTTP request smuggling via CRLF
50. Buffer exhaustion via oversized payload
51. Integer overflow in size calculations
52. Path traversal in file paths
53. Header injection attacks
54. DoS via rapid message flooding
55. Combined multi-vector attacks

---

## Phase 2.3: Fuzzing Infrastructure

### File Location
`Fuzzers/FuzzIPCMessage.cpp`

### Fuzzing Strategy

The fuzzer will use libFuzzer to generate random IPC message inputs and verify that validation never crashes and always correctly categorizes inputs as valid or invalid.

**Fuzzing Campaigns**:

1. **String Length Fuzzing** (8 hours)
   - Mutate string lengths around boundary values
   - Verify MaxLength validation catches all oversized strings

2. **URL Scheme Fuzzing** (8 hours)
   - Try all possible URL schemes (file, data, gopher, etc.)
   - Verify AllowedSchemes only accepts whitelisted schemes

3. **CRLF Injection Fuzzing** (8 hours)
   - Mutate strings with \r, \n, \r\n, \n\r combinations
   - Verify NoCRLF catches all variations

4. **Combined Attribute Fuzzing** (24 hours)
   - Random combinations of all attack vectors
   - Verify no validation bypasses exist

---

## Test Implementation Examples

### Example 1: Parser Test

```cpp
TEST_CASE(ipc_compiler_parses_max_length_attribute)
{
    auto ipc_source = R"~~~(
        endpoint TestEndpoint {
            test_message([MaxLength=256] ByteString field) =|
        }
    )~~~";
    
    auto endpoints = parse_ipc(ipc_source);
    VERIFY(endpoints.size() == 1);
    
    auto const& message = endpoints[0].messages[0];
    VERIFY(message.inputs.size() == 1);
    
    auto const& param = message.inputs[0];
    VERIFY(param.validation.max_length.has_value());
    EXPECT_EQ(param.validation.max_length.value(), 256u);
}
```

### Example 2: Integration Test

```cpp
TEST_CASE(validation_rejects_oversized_string)
{
    auto oversized_string = ByteString::repeated('A', 300);
    
    auto result = TestEndpoint::Messages::TestMessage::decode(
        encoder.encode(oversized_string)
    );
    
    EXPECT(result.is_error());
    EXPECT(result.error().string_literal().contains("exceeds maximum length"));
}
```

### Example 3: Fuzzer Target

```cpp
extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    auto decoder = IPC::Decoder::create(ByteBuffer::copy(data, size).release_value());
    auto result = RequestServer::Messages::StartRequest::decode(decoder);
    
    // Should not crash regardless of input
    return 0;
}
```

---

## Test Execution Plan

### Step 1: Create Test Files (30 minutes)

```bash
cd Tests/LibIPC
touch TestIPCCompiler.cpp
touch TestValidation.cpp

cd ../../Fuzzers
touch FuzzIPCMessage.cpp
```

### Step 2: Implement Parser Tests (2 hours)

Implement all 25 parser tests in `TestIPCCompiler.cpp` following the structure outlined above.

### Step 3: Implement Integration Tests (2 hours)

Implement all 30 integration tests in `TestValidation.cpp` following the structure outlined above.

### Step 4: Implement Fuzzer (30 minutes)

Create fuzzer target in `FuzzIPCMessage.cpp` and fuzzing dictionary `Fuzzers/ipc.dict`.

### Step 5: Build and Execute Tests (30 minutes)

```bash
# Build tests
cmake --preset Release
cmake --build --preset Release --target TestIPCCompiler TestValidation

# Run unit tests
./Build/release/Tests/LibIPC/TestIPCCompiler
./Build/release/Tests/LibIPC/TestValidation

# Build and run fuzzer
cmake --preset Fuzzers
cmake --build --preset Fuzzers --target FuzzIPCMessage
./Build/fuzzers/bin/FuzzIPCMessage -max_total_time=86400 -jobs=4
```

---

## Success Criteria

### Phase 2.1: Unit Tests ✅
- [ ] All 25 parser tests pass
- [ ] Code coverage >95% for parser code
- [ ] All attribute types tested
- [ ] Error handling tested

### Phase 2.2: Integration Tests ✅
- [ ] All 30 integration tests pass
- [ ] All attack scenarios prevented
- [ ] Rate limiting enforced correctly
- [ ] Validation code executes as expected

### Phase 2.3: Fuzzing ✅
- [ ] 24-hour fuzzing campaign completed
- [ ] Zero crashes discovered
- [ ] Code coverage >90% of validation logic
- [ ] No validation bypasses found

---

## Next Steps After Phase 2

1. **Phase 3: Migration** (3 hours)
   - Apply validation attributes to Services/RequestServer/RequestServer.ipc
   - Apply validation attributes to Services/ImageDecoder/ImageDecoder.ipc
   - Apply validation attributes to Services/WebContent/WebContentServer.ipc
   - Remove manual validation code from ConnectionFromClient classes
   - Verify equivalent security guarantees

2. **Phase 4: Documentation** (2 hours)
   - Update Documentation/IPCValidationAttributes.md
   - Update Documentation/LibIPCPatterns.md
   - Update Documentation/AddNewIPCEndpoint.md
   - Add examples to .ipc templates

---

**Document Version**: 1.0  
**Last Updated**: 2025-10-23  
**Author**: Security Hardening Team  
**Review Status**: Ready for Implementation

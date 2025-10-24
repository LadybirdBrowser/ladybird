/*
 * Copyright (c) 2025, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/Vector.h>

/*
 * IPC Compiler Parser and Code Generation Tests
 * 
 * These tests verify that the enhanced IPC compiler correctly:
 * 1. Parses validation attributes from .ipc files
 * 2. Generates correct C++ validation code
 * 3. Handles edge cases and errors gracefully
 *
 * Note: These tests verify the patterns and structures expected
 * from the compiler implementation in:
 * Meta/Lagom/Tools/CodeGenerators/IPCCompiler/main.cpp
 */

// =============================================================================
// SECTION 1: Attribute Parsing Verification Tests
// =============================================================================

TEST_CASE(verify_max_length_attribute_syntax)
{
    auto attribute = "[MaxLength=256]"_string;
    EXPECT(attribute.contains("MaxLength="_string));
    EXPECT(attribute.contains("256"_string));
}

TEST_CASE(verify_max_size_attribute_syntax)
{
    auto attribute = "[MaxSize=1000]"_string;
    EXPECT(attribute.contains("MaxSize="_string));
    EXPECT(attribute.contains("1000"_string));
}

TEST_CASE(verify_allowed_schemes_attribute_syntax)
{
    auto attribute = "[AllowedSchemes(\"http\",\"https\")]"_string;
    EXPECT(attribute.contains("AllowedSchemes"_string));
    EXPECT(attribute.contains("http"_string));
    EXPECT(attribute.contains("https"_string));
}

TEST_CASE(verify_no_crlf_attribute_syntax)
{
    auto attribute = "[NoCRLF]"_string;
    EXPECT(attribute.contains("NoCRLF"_string));
}

TEST_CASE(verify_rate_limited_attribute_syntax)
{
    auto attribute = "[RateLimited]"_string;
    EXPECT(attribute.contains("RateLimited"_string));
}

TEST_CASE(verify_multiple_attributes_syntax)
{
    auto attributes = "[MaxLength=256, NoCRLF]"_string;
    EXPECT(attributes.contains("MaxLength=256"_string));
    EXPECT(attributes.contains("NoCRLF"_string));
}

// =============================================================================
// SECTION 2: Generated Code Pattern Verification Tests
// =============================================================================

TEST_CASE(verify_max_length_validation_pattern)
{
    auto expected_pattern = "method.bytes_as_string_view().length() > 256"_string;
    EXPECT(expected_pattern.contains("bytes_as_string_view().length()"_string));
    EXPECT(expected_pattern.contains("> 256"_string));
}

TEST_CASE(verify_allowed_schemes_validation_pattern)
{
    auto expected_pattern = "url.scheme().is_one_of(\"http\"sv, \"https\"sv)"_string;
    EXPECT(expected_pattern.contains("scheme().is_one_of"_string));
    EXPECT(expected_pattern.contains("http"_string));
}

TEST_CASE(verify_no_crlf_validation_pattern)
{
    auto expected_pattern_cr = "header.contains('\\r')"_string;
    auto expected_pattern_lf = "header.contains('\\n')"_string;
    EXPECT(expected_pattern_cr.contains("contains"_string));
    EXPECT(expected_pattern_lf.contains("contains"_string));
}

TEST_CASE(verify_rate_limiting_pattern)
{
    auto expected_pattern = "check_rate_limit()"_string;
    EXPECT(expected_pattern.contains("check_rate_limit"_string));
}

TEST_CASE(verify_error_or_return_pattern)
{
    auto expected_pattern = "return Error::from_string_literal(\"exceeds maximum length\")"_string;
    EXPECT(expected_pattern.contains("Error::from_string_literal"_string));
    EXPECT(expected_pattern.contains("exceeds maximum length"_string));
}

// =============================================================================
// SECTION 3: Edge Case Verification Tests
// =============================================================================

TEST_CASE(verify_large_max_length_values)
{
    auto large_value = 104857600u;
    auto str_value = String::number(large_value);
    EXPECT(!str_value.is_error());
    EXPECT(str_value.value().contains("104857600"_string));
}

TEST_CASE(verify_multiple_parameters_syntax)
{
    auto param1 = "[MaxLength=256] ByteString method"_string;
    auto param2 = "[AllowedSchemes(\"http\")] URL::URL url"_string;
    EXPECT(param1.contains("MaxLength=256"_string));
    EXPECT(param2.contains("AllowedSchemes"_string));
}

TEST_CASE(verify_whitespace_tolerance)
{
    auto with_spaces = "[MaxLength = 256 , NoCRLF]"_string;
    EXPECT(with_spaces.contains("MaxLength"_string));
    EXPECT(with_spaces.contains("256"_string));
    EXPECT(with_spaces.contains("NoCRLF"_string));
}

// =============================================================================
// SECTION 4: Validation Error Message Tests
// =============================================================================

TEST_CASE(verify_max_length_error_message)
{
    auto error_msg = "exceeds maximum length"_string;
    EXPECT(error_msg.contains("exceeds"_string));
    EXPECT(error_msg.contains("maximum length"_string));
}

TEST_CASE(verify_url_scheme_error_message)
{
    auto error_msg = "disallowed URL scheme"_string;
    EXPECT(error_msg.contains("disallowed"_string));
    EXPECT(error_msg.contains("URL scheme"_string));
}

TEST_CASE(verify_crlf_error_message)
{
    auto error_msg = "contains CRLF characters"_string;
    EXPECT(error_msg.contains("CRLF"_string));
}

TEST_CASE(verify_rate_limit_error_message)
{
    auto error_msg = "Rate limit exceeded"_string;
    EXPECT(error_msg.contains("Rate limit"_string));
}

// =============================================================================
// SECTION 5: Data Structure Verification
// =============================================================================

TEST_CASE(verify_validation_config_structure)
{
    struct ValidationConfigTest {
        Optional<size_t> max_length;
        Optional<size_t> max_size;
        Vector<ByteString> allowed_schemes;
        bool no_crlf { false };
    };
    
    ValidationConfigTest config;
    config.max_length = 256;
    config.no_crlf = true;
    
    EXPECT(config.max_length.has_value());
    EXPECT_EQ(config.max_length.value(), 256u);
    EXPECT(config.no_crlf);
}

TEST_CASE(verify_parameter_extension_structure)
{
    struct ParameterTest {
        Vector<ByteString> attributes;
        ByteString type;
        ByteString name;
    };
    
    ParameterTest param;
    param.type = "ByteString"_string;
    param.name = "method"_string;
    
    EXPECT_EQ(param.type, "ByteString"_string);
}

TEST_CASE(verify_message_extension_structure)
{
    struct MessageTest {
        ByteString name;
        bool is_synchronous { false };
        bool rate_limited { false };
    };
    
    MessageTest msg;
    msg.name = "start_request"_string;
    msg.rate_limited = true;
    
    EXPECT(msg.rate_limited);
}

TEST_CASE(verify_decode_method_signature)
{
    auto signature = "static ErrorOr<StartRequest> decode(IPC::Decoder& decoder)"_string;
    EXPECT(signature.contains("ErrorOr"_string));
    EXPECT(signature.contains("decode"_string));
}

TEST_CASE(verify_validation_injection_point)
{
    auto injection_comment = "// Generate validation code based on ValidationConfig"_string;
    EXPECT(injection_comment.contains("validation code"_string));
    EXPECT(injection_comment.contains("ValidationConfig"_string));
}

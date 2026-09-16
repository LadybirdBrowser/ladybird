/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2023-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Encoder.h>
#include <LibURL/Parser.h>
#include <LibURL/RustIntegration.h>

namespace URL {

// https://url.spec.whatwg.org/#concept-host-parser
Optional<Host> Parser::parse_host(StringView input, bool is_opaque)
{
    return RustIntegration::parse_host(input, is_opaque);
}

Optional<Host> Parser::parse_host(Utf16View input, bool is_opaque)
{
    return RustIntegration::parse_host(input, is_opaque);
}

// https://url.spec.whatwg.org/#string-percent-encode-after-encoding
String Parser::percent_encode_after_encoding(TextCodec::Encoder& encoder, StringView input, PercentEncodeSet percent_encode_set, bool space_as_plus)
{
    // 1. Let encodeOutput be an empty I/O queue.
    StringBuilder output;

    // 2. Set potentialError to the result of running encode or fail with inputQueue, encoder, and encodeOutput.
    MUST(encoder.process(
        Utf8View(input),

        // 3. For each byte of encodeOutput converted to a byte sequence:
        [&](u8 byte) -> ErrorOr<void> {
            // 1. If spaceAsPlus is true and byte is 0x20 (SP), then append U+002B (+) to output and continue.
            if (space_as_plus && byte == ' ') {
                output.append('+');
                return {};
            }

            // 2. Let isomorph be a code point whose value is byte’s value.
            u32 isomorph = byte;

            // 3. Assert: percentEncodeSet includes all non-ASCII code points.

            // 4. If isomorphic is not in percentEncodeSet, then append isomorph to output.
            if (!code_point_is_in_percent_encode_set(isomorph, percent_encode_set)) {
                output.append_code_point(isomorph);
            }

            // 5. Otherwise, percent-encode byte and append the result to output.
            else {
                output.appendff("%{:02X}", byte);
            }

            return {};
        },

        // 4. If potentialError is non-null, then append "%26%23", followed by the shortest sequence of ASCII digits
        //    representing potentialError in base ten, followed by "%3B", to output.
        [&](u32 error) -> ErrorOr<void> {
            output.appendff("%26%23{}%3B", error);
            return {};
        }));

    // 6. Return output.
    return MUST(output.to_string());
}

static FFI::RustUrlByteSlice rust_encoding(Optional<StringView> encoding)
{
    if (!encoding.has_value())
        return { nullptr, 0 };
    return { reinterpret_cast<u8 const*>(encoding->characters_without_null_termination()), encoding->length() };
}

static Optional<URL> basic_parse_impl(FFI::RustUrlInput input, Optional<URL const&> base_url, Optional<StringView> encoding)
{
    Optional<FFI::RustUrl> rust_base_url;
    if (base_url.has_value())
        rust_base_url = base_url->to_rust();

    Optional<URL> result;
    auto on_complete = [](void* context, FFI::RustUrl const* rust_url) {
        if (!rust_url)
            return;
        auto& result = *static_cast<Optional<URL>*>(context);
        result.emplace();
        result->set_from_rust(*rust_url);
    };
    FFI::rust_url_basic_parse(input, rust_base_url.has_value() ? &*rust_base_url : nullptr, rust_encoding(encoding), &result, on_complete);
    return result;
}

static bool basic_parse_with_state_override_impl(FFI::RustUrlInput input, URL& url, Parser::State state_override, Optional<StringView> encoding)
{
    auto rust_url = url.to_rust();
    return FFI::rust_url_basic_parse_with_state_override(input, &rust_url, static_cast<FFI::State>(to_underlying(state_override)), rust_encoding(encoding), &url, URL::set_from_rust_callback);
}

Optional<URL> Parser::basic_parse(StringView raw_input, Optional<URL const&> base_url, Optional<StringView> encoding)
{
    auto input = String::from_utf8_with_replacement_character(raw_input, String::WithBOMHandling::No);
    return basic_parse_impl({ input.bytes().data(), nullptr, input.bytes().size() }, base_url, encoding);
}

Optional<URL> Parser::basic_parse(Utf16View raw_input, Optional<URL const&> base_url, Optional<StringView> encoding)
{
    return basic_parse_impl(RustIntegration::rust_url_input(raw_input), base_url, encoding);
}

bool Parser::basic_parse(StringView raw_input, URL& url, State state_override, Optional<StringView> encoding)
{
    auto input = String::from_utf8_with_replacement_character(raw_input, String::WithBOMHandling::No);
    return basic_parse_with_state_override_impl({ input.bytes().data(), nullptr, input.bytes().size() }, url, state_override, encoding);
}

bool Parser::basic_parse(Utf16View raw_input, URL& url, State state_override, Optional<StringView> encoding)
{
    return basic_parse_with_state_override_impl(RustIntegration::rust_url_input(raw_input), url, state_override, encoding);
}

}

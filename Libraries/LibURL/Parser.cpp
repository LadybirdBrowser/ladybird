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

Optional<URL> Parser::basic_parse(StringView raw_input, Optional<URL const&> base_url, URL* url, Optional<State> state_override, Optional<StringView> encoding)
{
    return RustIntegration::parse_basic_url(raw_input, base_url, url, state_override, encoding);
}

}

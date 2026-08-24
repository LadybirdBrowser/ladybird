/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/CSS/CharacterTypes.h>
#include <LibWeb/CSS/Parser/RustTokenizer.h>

namespace Web::CSS::Parser {

// U+FFFD REPLACEMENT CHARACTER (�)
static constexpr u32 REPLACEMENT_CHARACTER = 0xFFFD;

Utf16String RustTokenizer::normalize_input(StringView input, StringView encoding, TokenizerInput tokenizer_input)
{
    // https://www.w3.org/TR/css-syntax-3/#css-filter-code-points
    auto standardized_encoding = TextCodec::get_standardized_encoding(encoding);
    VERIFY(standardized_encoding.has_value());
    auto decoder = TextCodec::decoder_for(encoding);
    VERIFY(decoder.has_value());

    auto decoded_input = [&] {
        if (tokenizer_input == TokenizerInput::DecodedText) {
            VERIFY(Utf8View { input }.validate());
            return String::from_utf8_without_validation(input.bytes());
        }
        if (standardized_encoding->equals_ignoring_ascii_case("utf-8"sv) && Utf8View { input }.validate(AllowLonelySurrogates::No)) {
            if (input.bytes().starts_with({ { 0xef, 0xbb, 0xbf } }))
                input = input.substring_view(3);
            return String::from_utf8_without_validation(input.bytes());
        }
        return MUST(decoder->to_utf8(input, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement));
    }();

    return normalize_input(Utf16String::from_utf8_without_validation(decoded_input));
}

bool RustTokenizer::input_needs_normalization(Utf16View input)
{
    for (auto code_point : input) {
        if (code_point == '\r' || code_point == '\f' || code_point == 0x00 || is_unicode_surrogate(code_point))
            return true;
    }
    return false;
}

Utf16String RustTokenizer::normalize_input(Utf16View input)
{
    // OPTIMIZATION: If the input doesn't contain any filterable characters, we can skip the filtering.
    if (!input_needs_normalization(input))
        return Utf16String::from_utf16(input);

    Utf16StringBuilder builder { input.length_in_code_units() };
    bool last_was_carriage_return = false;

    // To filter code points from a stream of (unfiltered) code points input:
    for (auto code_point : input) {
        // Replace any U+000D CARRIAGE RETURN (CR) code points,
        // U+000C FORM FEED (FF) code points,
        // or pairs of U+000D CARRIAGE RETURN (CR) followed by U+000A LINE FEED (LF)
        // in input by a single U+000A LINE FEED (LF) code point.
        if (code_point == '\r') {
            if (last_was_carriage_return) {
                builder.append_code_point('\n');
            } else {
                last_was_carriage_return = true;
            }
        } else {
            if (last_was_carriage_return)
                builder.append_code_point('\n');

            if (code_point == '\n') {
                if (!last_was_carriage_return)
                    builder.append_code_point('\n');

            } else if (code_point == '\f') {
                builder.append_code_point('\n');
                // Replace any U+0000 NULL or surrogate code points in input with U+FFFD REPLACEMENT CHARACTER (�).
            } else if (code_point == 0x00 || is_unicode_surrogate(code_point)) {
                builder.append_code_point(REPLACEMENT_CHARACTER);
            } else {
                builder.append_code_point(code_point);
            }

            last_was_carriage_return = false;
        }
    }

    return builder.to_string();
}

}

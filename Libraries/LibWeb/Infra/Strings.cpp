/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/FlyString.h>
#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::Infra {

// https://infra.spec.whatwg.org/#normalize-newlines
String normalize_newlines(String const& string)
{
    // To normalize newlines in a string, replace every U+000D CR U+000A LF code point pair with a single U+000A LF
    // code point, and then replace every remaining U+000D CR code point with a U+000A LF code point.
    if (!string.contains('\r'))
        return string;

    StringBuilder builder;
    GenericLexer lexer { string };

    while (!lexer.is_eof()) {
        builder.append(lexer.consume_until('\r'));

        if (lexer.peek() == '\r') {
            lexer.ignore(1 + static_cast<size_t>(lexer.peek(1) == '\n'));
            builder.append('\n');
        }
    }

    return MUST(builder.to_string());
}

// https://infra.spec.whatwg.org/#strip-and-collapse-ascii-whitespace
ErrorOr<String> strip_and_collapse_whitespace(StringView string)
{
    // Replace any sequence of one or more consecutive code points that are ASCII whitespace in the string with a single U+0020 SPACE code point.
    StringBuilder builder;
    for (auto code_point : Utf8View { string }) {
        if (Infra::is_ascii_whitespace(code_point)) {
            if (!builder.string_view().ends_with(' '))
                builder.append(' ');
            continue;
        }
        TRY(builder.try_append_code_point(code_point));
    }

    // ...and then remove any leading and trailing ASCII whitespace from that string.
    return String::from_utf8(builder.string_view().trim(Infra::ASCII_WHITESPACE));
}

// https://infra.spec.whatwg.org/#code-unit-prefix
bool is_code_unit_prefix(StringView potential_prefix_utf8, StringView input_utf8)
{
    auto potential_prefix_utf16_bytes = MUST(utf8_to_utf16(potential_prefix_utf8));
    auto input_utf16_bytes = MUST(utf8_to_utf16(input_utf8));
    Utf16View potential_prefix { potential_prefix_utf16_bytes };
    Utf16View input { input_utf16_bytes };

    // 1. Let i be 0.
    size_t i = 0;

    // 2. While true:
    while (true) {
        // 1. If i is greater than or equal to potentialPrefix’s length, then return true.
        if (i >= potential_prefix.length_in_code_units())
            return true;

        // 2. If i is greater than or equal to input’s length, then return false.
        if (i >= input.length_in_code_units())
            return false;

        // 3. Let potentialPrefixCodeUnit be the ith code unit of potentialPrefix.
        auto potential_prefix_code_unit = potential_prefix.code_unit_at(i);

        // 4. Let inputCodeUnit be the ith code unit of input.
        auto input_code_unit = input.code_unit_at(i);

        // 5. Return false if potentialPrefixCodeUnit is not inputCodeUnit.
        if (potential_prefix_code_unit != input_code_unit)
            return false;

        // 6. Set i to i + 1.
        ++i;
    }
}

// https://infra.spec.whatwg.org/#scalar-value-string
ErrorOr<String> convert_to_scalar_value_string(StringView string)
{
    // To convert a string into a scalar value string, replace any surrogates with U+FFFD.
    StringBuilder scalar_value_builder;
    auto utf8_view = Utf8View { string };
    for (u32 code_point : utf8_view) {
        if (is_unicode_surrogate(code_point))
            code_point = 0xFFFD;
        scalar_value_builder.append_code_point(code_point);
    }
    return scalar_value_builder.to_string();
}

// https://infra.spec.whatwg.org/#isomorphic-encode
ByteBuffer isomorphic_encode(StringView input)
{
    // To isomorphic encode an isomorphic string input: return a byte sequence whose length is equal to input’s code
    // point length and whose bytes have the same values as the values of input’s code points, in the same order.
    // NOTE: This is essentially spec-speak for "Encode as ISO-8859-1 / Latin-1".
    ByteBuffer buf = {};
    for (auto code_point : Utf8View { input }) {
        // VERIFY(code_point <= 0xFF);
        if (code_point > 0xFF)
            dbgln("FIXME: Trying to isomorphic encode a string with code points > U+00FF.");
        buf.append((u8)code_point);
    }
    return buf;
}

// https://infra.spec.whatwg.org/#isomorphic-decode
String isomorphic_decode(ReadonlyBytes input)
{
    // To isomorphic decode a byte sequence input, return a string whose code point length is equal
    // to input’s length and whose code points have the same values as the values of input’s bytes, in the same order.
    // NOTE: This is essentially spec-speak for "Decode as ISO-8859-1 / Latin-1".
    StringBuilder builder(input.size());
    for (u8 code_point : input) {
        builder.append_code_point(code_point);
    }
    return builder.to_string_without_validation();
}

// https://infra.spec.whatwg.org/#code-unit-less-than
bool code_unit_less_than(StringView a, StringView b)
{
    // FIXME: Perhaps there is a faster way to do this?

    // Fastpath for ASCII-only strings.
    if (a.is_ascii() && b.is_ascii())
        return a < b;

    auto a_utf16 = MUST(utf8_to_utf16(a));
    auto b_utf16 = MUST(utf8_to_utf16(b));
    return Utf16View { a_utf16 }.is_code_unit_less_than(Utf16View { b_utf16 });
}

}

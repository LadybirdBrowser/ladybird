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

// https://infra.spec.whatwg.org/#ascii-case-insensitive
bool is_ascii_case_insensitive_match(StringView a, StringView b)
{
    // A string A is an ASCII case-insensitive match for a string B,
    // if the ASCII lowercase of A is the ASCII lowercase of B.
    return AK::StringUtils::equals_ignoring_ascii_case(a, b);
}

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
bool is_code_unit_prefix(StringView potential_prefix, StringView input)
{
    auto potential_prefix_utf16 = utf8_to_utf16(potential_prefix).release_value_but_fixme_should_propagate_errors();
    auto input_utf16 = utf8_to_utf16(input).release_value_but_fixme_should_propagate_errors();

    // 1. Let i be 0.
    size_t i = 0;

    // 2. While true:
    while (true) {
        // 1. If i is greater than or equal to potentialPrefix’s length, then return true.
        if (i >= potential_prefix.length())
            return true;

        // 2. If i is greater than or equal to input’s length, then return false.
        if (i >= input.length())
            return false;

        // 3. Let potentialPrefixCodeUnit be the ith code unit of potentialPrefix.
        auto potential_prefix_code_unit = Utf16View(potential_prefix_utf16).code_unit_at(i);

        // 4. Let inputCodeUnit be the ith code unit of input.
        auto input_code_unit = Utf16View(input_utf16).code_unit_at(i);

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

// https://infra.spec.whatwg.org/#ascii-lowercase
ErrorOr<String> to_ascii_lowercase(StringView string)
{
    // To ASCII lowercase a string, replace all ASCII upper alphas in the string with their
    // corresponding code point in ASCII lower alpha.
    StringBuilder string_builder;
    auto utf8_view = Utf8View { string };
    for (u32 code_point : utf8_view) {
        code_point = AK::to_ascii_lowercase(code_point);
        string_builder.append_code_point(code_point);
    }
    return string_builder.to_string();
}

// https://infra.spec.whatwg.org/#ascii-uppercase
ErrorOr<String> to_ascii_uppercase(StringView string)
{
    // To ASCII uppercase a string, replace all ASCII lower alphas in the string with their
    // corresponding code point in ASCII upper alpha.
    StringBuilder string_builder;
    auto utf8_view = Utf8View { string };
    for (u32 code_point : utf8_view) {
        code_point = AK::to_ascii_uppercase(code_point);
        string_builder.append_code_point(code_point);
    }
    return string_builder.to_string();
}

}

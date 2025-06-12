/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibURL/Pattern/PatternError.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#token
// A token is a struct representing a single lexical token within a pattern string.
struct Token {
    // https://urlpattern.spec.whatwg.org/#token-type
    enum class Type {
        // The token represents a U+007B ({) code point.
        Open,

        // The token represents a U+007D (}) code point.
        Close,

        // The token represents a string of the form "(<regular expression>)". The regular expression is required to consist of only ASCII code points.
        Regexp,

        // The token represents a string of the form ":<name>". The name value is restricted to code points that are consistent with JavaScript identifiers.
        Name,

        // The token represents a valid pattern code point without any special syntactical meaning.
        Char,

        // The token represents a code point escaped using a backslash like "\<char>".
        EscapedChar,

        // The token represents a matching group modifier that is either the U+003F (?) or U+002B (+) code points.
        OtherModifier,

        // The token represents a U+002A (*) code point that can be either a wildcard matching group or a matching group modifier.
        Asterisk,

        // The token represents the end of the pattern string.
        End,

        // The token represents a code point that is invalid in the pattern. This could be because of the code point value
        // itself or due to its location within the pattern relative to other syntactic elements.
        InvalidChar,
    };

    // https://urlpattern.spec.whatwg.org/#token-type
    // A token has an associated type, a string, initially "invalid-char".
    Type type { Type::InvalidChar };

    // https://urlpattern.spec.whatwg.org/#token-index
    // A token has an associated index, a number, initially 0. It is the position of the first code point in the pattern string represented by the token.
    u32 index { 0 };

    // https://urlpattern.spec.whatwg.org/#token-value
    // A token has an associated value, a string, initially the empty string. It contains the code points from the pattern string represented by the token.
    String value;

    String to_string() const;
    static StringView type_to_string(Token::Type);
};

// https://urlpattern.spec.whatwg.org/#tokenizer
// A tokenizer is a struct.
class Tokenizer {
public:
    // https://urlpattern.spec.whatwg.org/#tokenize-policy
    // A tokenize policy is a string that must be either "strict" or "lenient".
    enum class Policy {
        Strict,
        Lenient,
    };

    static PatternErrorOr<Vector<Token>> tokenize(Utf8View const&, Policy);

    static bool is_a_valid_name_code_point(u32 code_point, bool first);

private:
    Tokenizer(Utf8View const& input, Policy);

    void get_the_next_code_point();
    void seek_and_get_the_next_code_point(u32 index);
    void add_a_token(Token::Type, u32 next_position, u32 value_position, u32 value_length);
    void add_a_token_with_default_length(Token::Type, u32 next_position, u32 value_position);
    void add_a_token_with_default_position_and_length(Token::Type);
    PatternErrorOr<void> process_a_tokenizing_error(u32 next_position, u32 value_position);

    // https://urlpattern.spec.whatwg.org/#tokenizer-input
    // A tokenizer has an associated input, a pattern string, initially the empty string.
    Utf8View m_input;

    // https://urlpattern.spec.whatwg.org/#tokenizer-policy
    // A tokenizer has an associated policy, a tokenize policy, initially "strict".
    Policy m_policy { Policy::Strict };

    // https://urlpattern.spec.whatwg.org/#tokenizer-token-list
    // A tokenizer has an associated token list, a token list, initially an empty list.
    Vector<Token> m_token_list;

    // https://urlpattern.spec.whatwg.org/#tokenizer-index
    // A tokenizer has an associated index, a number, initially 0.
    size_t m_index { 0 };

    // https://urlpattern.spec.whatwg.org/#tokenizer-next-index
    // A tokenizer has an associated next index, a number, initially 0.
    size_t m_next_index { 0 };

    // https://urlpattern.spec.whatwg.org/#tokenizer-code-point
    // A tokenizer has an associated code point, a Unicode code point, initially null.
    u32 m_code_point {};
};

}

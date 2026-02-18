/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibURL/Pattern/Options.h>
#include <LibURL/Pattern/Part.h>
#include <LibURL/Pattern/PatternError.h>
#include <LibURL/Pattern/Tokenizer.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#pattern-parser
class PatternParser {
public:
    // https://urlpattern.spec.whatwg.org/#encoding-callback
    // An encoding callback is an abstract algorithm that takes a given string input. The input will be a simple text
    // piece of a pattern string. An implementing algorithm will validate and encode the input. It must return the
    // encoded string or throw an exception.
    using EncodingCallback = Function<PatternErrorOr<String>(String const&)>;

    static PatternErrorOr<Vector<Part>> parse(Utf8View const& input, Options const&, EncodingCallback);

private:
    PatternParser(EncodingCallback, String segment_wildcard_regexp);

    Optional<Token const&> try_to_consume_a_token(Token::Type);
    Optional<Token const&> try_to_consume_a_modifier_token();
    Optional<Token const&> try_to_consume_a_regexp_or_wildcard_token(Optional<Token const&> name_token);
    PatternErrorOr<void> consume_a_required_token(Token::Type);
    String consume_text();
    PatternErrorOr<void> maybe_add_a_part_from_the_pending_fixed_value();
    PatternErrorOr<void> add_a_part(
        String const& prefix,
        Optional<Token const&> name_token,
        Optional<Token const&> regexp_or_wildcard_token,
        String const& suffix,
        Optional<Token const&> modifier_token);
    bool is_a_duplicate_name(String const&) const;

    // https://urlpattern.spec.whatwg.org/#pattern-parser-token-list
    // A pattern parser has an associated token list, a token list, initially an empty list.
    Vector<Token> m_token_list;

    // https://urlpattern.spec.whatwg.org/#pattern-parser-encoding-callback
    // A pattern parser has an associated encoding callback, a encoding callback, that must be set upon creation.
    EncodingCallback m_encoding_callback;

    // https://urlpattern.spec.whatwg.org/#pattern-parser-segment-wildcard-regexp
    // A pattern parser has an associated segment wildcard regexp, a string, that must be set upon creation.
    String m_segment_wildcard_regexp;

    // https://urlpattern.spec.whatwg.org/#pattern-parser-part-list
    // A pattern parser has an associated part list, a part list, initially an empty list.
    Vector<Part> m_part_list;

    // https://urlpattern.spec.whatwg.org/#pattern-parser-pending-fixed-value
    // A pattern parser has an associated pending fixed value, a string, initially the empty string.
    StringBuilder m_pending_fixed_value;

    // https://urlpattern.spec.whatwg.org/#pattern-parser-index
    // A pattern parser has an associated index, a number, initially 0.
    size_t m_index { 0 };

    // https://urlpattern.spec.whatwg.org/#pattern-parser-next-numeric-name
    // A pattern parser has an associated next numeric name, a number, initially 0.
    size_t m_next_numeric_name { 0 };
};

}

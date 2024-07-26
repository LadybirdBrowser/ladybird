/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Utf8View.h>
#include <LibWeb/CSS/Parser/Token.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

class U32Twin {
public:
    void set(size_t index, u32 value)
    {
        if (index == 0)
            first = value;
        if (index == 1)
            second = value;
    }

    u32 first {};
    u32 second {};
};

class U32Triplet {
public:
    void set(size_t index, u32 value)
    {
        if (index == 0)
            first = value;
        if (index == 1)
            second = value;
        if (index == 2)
            third = value;
    }

    U32Twin to_twin_12()
    {
        return { first, second };
    }

    U32Twin to_twin_23()
    {
        return { second, third };
    }

    u32 first {};
    u32 second {};
    u32 third {};
};

class Tokenizer {
public:
    static Vector<Token> tokenize(StringView input, StringView encoding);

    [[nodiscard]] static Token create_eof_token();

private:
    explicit Tokenizer(String decoded_input);

    [[nodiscard]] Vector<Token> tokenize();

    size_t current_byte_offset() const;
    String input_since(size_t offset) const;

    [[nodiscard]] u32 next_code_point();
    [[nodiscard]] u32 peek_code_point(size_t offset = 0) const;
    [[nodiscard]] U32Twin peek_twin() const;
    [[nodiscard]] U32Triplet peek_triplet() const;

    [[nodiscard]] U32Twin start_of_input_stream_twin();
    [[nodiscard]] U32Triplet start_of_input_stream_triplet();

    [[nodiscard]] static Token create_new_token(Token::Type);
    [[nodiscard]] static Token create_value_token(Token::Type, FlyString&& value, String&& representation);
    [[nodiscard]] static Token create_value_token(Token::Type, u32 value, String&& representation);
    [[nodiscard]] Token consume_a_token();
    [[nodiscard]] Token consume_string_token(u32 ending_code_point);
    [[nodiscard]] Token consume_a_numeric_token();
    [[nodiscard]] Token consume_an_ident_like_token();
    [[nodiscard]] Number consume_a_number();
    [[nodiscard]] double convert_a_string_to_a_number(StringView);
    [[nodiscard]] FlyString consume_an_ident_sequence();
    [[nodiscard]] u32 consume_escaped_code_point();
    [[nodiscard]] Token consume_a_url_token();
    void consume_the_remnants_of_a_bad_url();
    void consume_comments();
    void consume_as_much_whitespace_as_possible();
    void reconsume_current_input_code_point();
    [[nodiscard]] static bool is_valid_escape_sequence(U32Twin);
    [[nodiscard]] static bool would_start_an_ident_sequence(U32Triplet);
    [[nodiscard]] static bool would_start_a_number(U32Triplet);

    String m_decoded_input;
    Utf8View m_utf8_view;
    AK::Utf8CodePointIterator m_utf8_iterator;
    AK::Utf8CodePointIterator m_prev_utf8_iterator;
    Token::Position m_position;
    Token::Position m_prev_position;
};
}

/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/FloatingPointStringConversions.h>
#include <AK/SourceLocation.h>
#include <AK/Vector.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/CSS/Parser/Tokenizer.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS::Parser {

// U+FFFD REPLACEMENT CHARACTER (�)
#define REPLACEMENT_CHARACTER 0xFFFD
static constexpr u32 TOKENIZER_EOF = 0xFFFFFFFF;

static inline void log_parse_error(SourceLocation const& location = SourceLocation::current())
{
    dbgln_if(CSS_TOKENIZER_DEBUG, "Parse error (css tokenization) {} ", location);
}

static inline bool is_eof(u32 code_point)
{
    return code_point == TOKENIZER_EOF;
}

static inline bool is_quotation_mark(u32 code_point)
{
    return code_point == 0x22;
}

static inline bool is_greater_than_maximum_allowed_code_point(u32 code_point)
{
    return code_point > 0x10FFFF;
}

static inline bool is_low_line(u32 code_point)
{
    return code_point == 0x5F;
}

// https://www.w3.org/TR/css-syntax-3/#ident-start-code-point
static inline bool is_ident_start_code_point(u32 code_point)
{
    // FIXME: We use !is_ascii() for "non-ASCII code point" in the spec, but it's not quite right -
    //        it treats EOF as a valid! The spec also lacks a definition of code point. For now, the
    //        !is_eof() check is a hack, but it should work.
    return !is_eof(code_point) && (is_ascii_alpha(code_point) || !is_ascii(code_point) || is_low_line(code_point));
}

static inline bool is_hyphen_minus(u32 code_point)
{
    return code_point == 0x2D;
}

// https://www.w3.org/TR/css-syntax-3/#ident-code-point
static inline bool is_ident_code_point(u32 code_point)
{
    return is_ident_start_code_point(code_point) || is_ascii_digit(code_point) || is_hyphen_minus(code_point);
}

static inline bool is_non_printable(u32 code_point)
{
    return code_point <= 0x8 || code_point == 0xB || (code_point >= 0xE && code_point <= 0x1F) || code_point == 0x7F;
}

static inline bool is_number_sign(u32 code_point)
{
    return code_point == 0x23;
}

static inline bool is_reverse_solidus(u32 code_point)
{
    return code_point == 0x5C;
}

static inline bool is_apostrophe(u32 code_point)
{
    return code_point == 0x27;
}

static inline bool is_left_paren(u32 code_point)
{
    return code_point == 0x28;
}

static inline bool is_right_paren(u32 code_point)
{
    return code_point == 0x29;
}

static inline bool is_plus_sign(u32 code_point)
{
    return code_point == 0x2B;
}

static inline bool is_comma(u32 code_point)
{
    return code_point == 0x2C;
}

static inline bool is_full_stop(u32 code_point)
{
    return code_point == 0x2E;
}

static inline bool is_newline(u32 code_point)
{
    return code_point == 0xA;
}

static inline bool is_asterisk(u32 code_point)
{
    return code_point == 0x2A;
}

static inline bool is_solidus(u32 code_point)
{
    return code_point == 0x2F;
}

static inline bool is_colon(u32 code_point)
{
    return code_point == 0x3A;
}

static inline bool is_semicolon(u32 code_point)
{
    return code_point == 0x3B;
}

static inline bool is_less_than_sign(u32 code_point)
{
    return code_point == 0x3C;
}

static inline bool is_greater_than_sign(u32 code_point)
{
    return code_point == 0x3E;
}

static inline bool is_at(u32 code_point)
{
    return code_point == 0x40;
}

static inline bool is_open_square_bracket(u32 code_point)
{
    return code_point == 0x5B;
}

static inline bool is_closed_square_bracket(u32 code_point)
{
    return code_point == 0x5D;
}

static inline bool is_open_curly_bracket(u32 code_point)
{
    return code_point == 0x7B;
}

static inline bool is_closed_curly_bracket(u32 code_point)
{
    return code_point == 0x7D;
}

static inline bool is_whitespace(u32 code_point)
{
    return code_point == 0x9 || code_point == 0xA || code_point == 0x20;
}

static inline bool is_percent(u32 code_point)
{
    return code_point == 0x25;
}

static inline bool is_exclamation_mark(u32 code_point)
{
    return code_point == 0x21;
}

static inline bool is_e(u32 code_point)
{
    return code_point == 0x65;
}

static inline bool is_E(u32 code_point)
{
    return code_point == 0x45;
}

Vector<Token> Tokenizer::tokenize(StringView input, StringView encoding)
{
    // https://www.w3.org/TR/css-syntax-3/#css-filter-code-points
    auto filter_code_points = [](StringView input, auto encoding) -> String {
        auto decoder = TextCodec::decoder_for(encoding);
        VERIFY(decoder.has_value());

        auto decoded_input = MUST(decoder->to_utf8(input));

        // OPTIMIZATION: If the input doesn't contain any CR or FF, we can skip the filtering
        bool const contains_cr_or_ff = [&] {
            for (auto byte : decoded_input.bytes()) {
                if (byte == '\r' || byte == '\f')
                    return true;
            }
            return false;
        }();
        if (!contains_cr_or_ff) {
            return decoded_input;
        }

        StringBuilder builder { input.length() };
        bool last_was_carriage_return = false;

        // To filter code points from a stream of (unfiltered) code points input:
        for (auto code_point : decoded_input.code_points()) {
            // Replace any U+000D CARRIAGE RETURN (CR) code points,
            // U+000C FORM FEED (FF) code points,
            // or pairs of U+000D CARRIAGE RETURN (CR) followed by U+000A LINE FEED (LF)
            // in input by a single U+000A LINE FEED (LF) code point.
            if (code_point == '\r') {
                if (last_was_carriage_return) {
                    builder.append('\n');
                } else {
                    last_was_carriage_return = true;
                }
            } else {
                if (last_was_carriage_return)
                    builder.append('\n');

                if (code_point == '\n') {
                    if (!last_was_carriage_return)
                        builder.append('\n');

                } else if (code_point == '\f') {
                    builder.append('\n');
                    // Replace any U+0000 NULL or surrogate code points in input with U+FFFD REPLACEMENT CHARACTER (�).
                } else if (code_point == 0x00 || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
                    builder.append_code_point(REPLACEMENT_CHARACTER);
                } else {
                    builder.append_code_point(code_point);
                }

                last_was_carriage_return = false;
            }
        }
        return builder.to_string_without_validation();
    };

    Tokenizer tokenizer { filter_code_points(input, encoding) };
    return tokenizer.tokenize();
}

Tokenizer::Tokenizer(String decoded_input)
    : m_decoded_input(move(decoded_input))
    , m_utf8_view(m_decoded_input)
    , m_utf8_iterator(m_utf8_view.begin())
{
}

Vector<Token> Tokenizer::tokenize()
{
    Vector<Token> tokens;
    for (;;) {
        auto token_start = m_position;
        auto token = consume_a_token();
        token.m_start_position = token_start;
        token.m_end_position = m_position;
        tokens.append(token);

        if (token.is(Token::Type::EndOfFile)) {
            return tokens;
        }
    }
}

u32 Tokenizer::next_code_point()
{
    if (m_utf8_iterator == m_utf8_view.end())
        return TOKENIZER_EOF;
    m_prev_utf8_iterator = m_utf8_iterator;
    ++m_utf8_iterator;
    auto code_point = *m_prev_utf8_iterator;

    m_prev_position = m_position;
    if (is_newline(code_point)) {
        m_position.line++;
        m_position.column = 0;
    } else {
        m_position.column++;
    }

    dbgln_if(CSS_TOKENIZER_DEBUG, "(Tokenizer) Next code_point: {:d}", code_point);
    return code_point;
}

u32 Tokenizer::peek_code_point(size_t offset) const
{
    auto it = m_utf8_iterator;
    for (size_t i = 0; i < offset && it != m_utf8_view.end(); ++i)
        ++it;
    if (it == m_utf8_view.end())
        return TOKENIZER_EOF;
    dbgln_if(CSS_TOKENIZER_DEBUG, "(Tokenizer) Peek code_point: {:d}", *m_prev_utf8_iterator);
    return *it;
}

U32Twin Tokenizer::peek_twin() const
{
    U32Twin values { TOKENIZER_EOF, TOKENIZER_EOF };
    auto it = m_utf8_iterator;
    for (size_t i = 0; i < 2 && it != m_utf8_view.end(); ++i) {
        values.set(i, *it);
        ++it;
    }
    dbgln_if(CSS_TOKENIZER_DEBUG, "(Tokenizer) Peek twin: {:d},{:d}", values.first, values.second);
    return values;
}

U32Triplet Tokenizer::peek_triplet() const
{
    U32Triplet values { TOKENIZER_EOF, TOKENIZER_EOF, TOKENIZER_EOF };
    auto it = m_utf8_iterator;
    for (size_t i = 0; i < 3 && it != m_utf8_view.end(); ++i) {
        values.set(i, *it);
        ++it;
    }
    dbgln_if(CSS_TOKENIZER_DEBUG, "(Tokenizer) Peek triplet: {:d},{:d},{:d}", values.first, values.second, values.third);
    return values;
}

U32Twin Tokenizer::start_of_input_stream_twin()
{
    U32Twin twin;
    // FIXME: Reconsuming just to read the current code point again is weird.
    reconsume_current_input_code_point();
    twin.first = next_code_point();
    twin.second = peek_code_point();

    return twin;
}

U32Triplet Tokenizer::start_of_input_stream_triplet()
{
    U32Triplet triplet;
    // FIXME: Reconsuming just to read the current code point again is weird.
    reconsume_current_input_code_point();
    triplet.first = next_code_point();
    auto next_two = peek_twin();
    triplet.second = next_two.first;
    triplet.third = next_two.second;

    return triplet;
}

Token Tokenizer::create_new_token(Token::Type type)
{
    Token token = {};
    token.m_type = type;
    return token;
}

Token Tokenizer::create_eof_token()
{
    return create_new_token(Token::Type::EndOfFile);
}

Token Tokenizer::create_value_token(Token::Type type, FlyString&& value, String&& representation)
{
    auto token = create_new_token(type);
    token.m_value = move(value);
    token.m_original_source_text = move(representation);
    return token;
}

Token Tokenizer::create_value_token(Token::Type type, u32 value, String&& representation)
{
    auto token = create_new_token(type);
    token.m_value = String::from_code_point(value);
    token.m_original_source_text = move(representation);
    return token;
}

// https://www.w3.org/TR/css-syntax-3/#consume-escaped-code-point
u32 Tokenizer::consume_escaped_code_point()
{
    // This section describes how to consume an escaped code point.
    // It assumes that the U+005C REVERSE SOLIDUS (\) has already been consumed and that the next
    // input code point has already been verified to be part of a valid escape.
    // It will return a code point.

    // Consume the next input code point.
    auto input = next_code_point();

    // hex digit
    if (is_ascii_hex_digit(input)) {
        // Consume as many hex digits as possible, but no more than 5.
        // Note that this means 1-6 hex digits have been consumed in total.
        StringBuilder builder;
        builder.append_code_point(input);

        size_t counter = 0;
        while (is_ascii_hex_digit(peek_code_point()) && counter++ < 5) {
            builder.append_code_point(next_code_point());
        }

        // If the next input code point is whitespace, consume it as well.
        if (is_whitespace(peek_code_point())) {
            (void)next_code_point();
        }

        // Interpret the hex digits as a hexadecimal number.
        auto unhexed = AK::StringUtils::convert_to_uint_from_hex<u32>(builder.string_view()).value_or(0);
        // If this number is zero, or is for a surrogate, or is greater than the maximum allowed
        // code point, return U+FFFD REPLACEMENT CHARACTER (�).
        if (unhexed == 0 || is_unicode_surrogate(unhexed) || is_greater_than_maximum_allowed_code_point(unhexed)) {
            return REPLACEMENT_CHARACTER;
        }

        // Otherwise, return the code point with that value.
        return unhexed;
    }

    // EOF
    if (is_eof(input)) {
        // This is a parse error. Return U+FFFD REPLACEMENT CHARACTER (�).
        log_parse_error();
        return REPLACEMENT_CHARACTER;
    }

    // anything else
    // Return the current input code point.
    return input;
}

// https://www.w3.org/TR/css-syntax-3/#consume-ident-like-token
Token Tokenizer::consume_an_ident_like_token()
{
    // This section describes how to consume an ident-like token from a stream of code points.
    // It returns an <ident-token>, <function-token>, <url-token>, or <bad-url-token>.

    // Consume an ident sequence, and let string be the result.
    auto start_byte_offset = current_byte_offset();
    auto string = consume_an_ident_sequence();

    // If string’s value is an ASCII case-insensitive match for "url", and the next input code
    // point is U+0028 LEFT PARENTHESIS ((), consume it.
    if (Infra::is_ascii_case_insensitive_match(string, "url"sv) && is_left_paren(peek_code_point())) {
        (void)next_code_point();

        // While the next two input code points are whitespace, consume the next input code point.
        for (;;) {
            auto maybe_whitespace = peek_twin();
            if (!(is_whitespace(maybe_whitespace.first) && is_whitespace(maybe_whitespace.second))) {
                break;
            }

            (void)next_code_point();
        }

        // If the next one or two input code points are U+0022 QUOTATION MARK ("), U+0027 APOSTROPHE ('),
        // or whitespace followed by U+0022 QUOTATION MARK (") or U+0027 APOSTROPHE ('), then create a
        // <function-token> with its value set to string and return it.
        auto next_two = peek_twin();
        if (is_quotation_mark(next_two.first) || is_apostrophe(next_two.first) || (is_whitespace(next_two.first) && (is_quotation_mark(next_two.second) || is_apostrophe(next_two.second)))) {
            return create_value_token(Token::Type::Function, move(string), input_since(start_byte_offset));
        }

        // Otherwise, consume a url token, and return it.
        return consume_a_url_token();
    }

    // Otherwise, if the next input code point is U+0028 LEFT PARENTHESIS ((), consume it.
    if (is_left_paren(peek_code_point())) {
        (void)next_code_point();

        // Create a <function-token> with its value set to string and return it.
        return create_value_token(Token::Type::Function, move(string), input_since(start_byte_offset));
    }

    // Otherwise, create an <ident-token> with its value set to string and return it.
    return create_value_token(Token::Type::Ident, move(string), input_since(start_byte_offset));
}

// https://www.w3.org/TR/css-syntax-3/#consume-number
Number Tokenizer::consume_a_number()
{
    // This section describes how to consume a number from a stream of code points.
    // It returns a numeric value, and a type which is either "integer" or "number".
    //
    // Note: This algorithm does not do the verification of the first few code points
    // that are necessary to ensure a number can be obtained from the stream. Ensure
    // that the stream starts with a number before calling this algorithm.

    // Execute the following steps in order:

    // 1. Initially set type to "integer". Let repr be the empty string.
    StringBuilder repr;
    Number::Type type = Number::Type::Integer;

    // 2. If the next input code point is U+002B PLUS SIGN (+) or U+002D HYPHEN-MINUS (-),
    // consume it and append it to repr.
    bool has_explicit_sign = false;
    auto next_input = peek_code_point();
    if (is_plus_sign(next_input) || is_hyphen_minus(next_input)) {
        has_explicit_sign = true;
        repr.append_code_point(next_code_point());
    }

    // 3. While the next input code point is a digit, consume it and append it to repr.
    for (;;) {
        auto digits = peek_code_point();
        if (!is_ascii_digit(digits))
            break;

        repr.append_code_point(next_code_point());
    }

    // 4. If the next 2 input code points are U+002E FULL STOP (.) followed by a digit, then:
    auto maybe_number = peek_twin();
    if (is_full_stop(maybe_number.first) && is_ascii_digit(maybe_number.second)) {
        // 1. Consume them.
        // 2. Append them to repr.
        repr.append_code_point(next_code_point());
        repr.append_code_point(next_code_point());

        // 3. Set type to "number".
        type = Number::Type::Number;

        // 4. While the next input code point is a digit, consume it and append it to repr.
        for (;;) {
            auto digit = peek_code_point();
            if (!is_ascii_digit(digit))
                break;

            repr.append_code_point(next_code_point());
        }
    }

    // 5. If the next 2 or 3 input code points are U+0045 LATIN CAPITAL LETTER E (E) or
    // U+0065 LATIN SMALL LETTER E (e), optionally followed by U+002D HYPHEN-MINUS (-)
    // or U+002B PLUS SIGN (+), followed by a digit, then:
    auto maybe_exp = peek_triplet();
    if ((is_E(maybe_exp.first) || is_e(maybe_exp.first))
        && (((is_plus_sign(maybe_exp.second) || is_hyphen_minus(maybe_exp.second)) && is_ascii_digit(maybe_exp.third))
            || (is_ascii_digit(maybe_exp.second)))) {
        // 1. Consume them.
        // 2. Append them to repr.
        if (is_plus_sign(maybe_exp.second) || is_hyphen_minus(maybe_exp.second)) {
            if (is_ascii_digit(maybe_exp.third)) {
                repr.append_code_point(next_code_point());
                repr.append_code_point(next_code_point());
                repr.append_code_point(next_code_point());
            }
        } else if (is_ascii_digit(maybe_exp.second)) {
            repr.append_code_point(next_code_point());
            repr.append_code_point(next_code_point());
        }

        // 3. Set type to "number".
        type = Number::Type::Number;

        // 4. While the next input code point is a digit, consume it and append it to repr.
        for (;;) {
            auto digits = peek_code_point();
            if (!is_ascii_digit(digits))
                break;

            repr.append_code_point(next_code_point());
        }
    }

    // 6. Convert repr to a number, and set the value to the returned value.
    auto value = convert_a_string_to_a_number(repr.string_view());

    // 7. Return value and type.
    if (type == Number::Type::Integer && has_explicit_sign)
        return Number { Number::Type::IntegerWithExplicitSign, value };
    return Number { type, value };
}

// https://www.w3.org/TR/css-syntax-3/#convert-string-to-number
double Tokenizer::convert_a_string_to_a_number(StringView string)
{
    // FIXME: We already found the whole part, fraction part and exponent during
    //        validation, we could probably skip
    return string.to_number<double>(AK::TrimWhitespace::No).release_value();
}

// https://www.w3.org/TR/css-syntax-3/#consume-name
FlyString Tokenizer::consume_an_ident_sequence()
{
    // This section describes how to consume an ident sequence from a stream of code points.
    // It returns a string containing the largest name that can be formed from adjacent
    // code points in the stream, starting from the first.
    //
    // Note: This algorithm does not do the verification of the first few code points that
    // are necessary to ensure the returned code points would constitute an <ident-token>.
    // If that is the intended use, ensure that the stream starts with an ident sequence before
    // calling this algorithm.

    // Let result initially be an empty string.
    StringBuilder result;

    // Repeatedly consume the next input code point from the stream:
    for (;;) {
        auto input = next_code_point();

        if (is_eof(input))
            break;

        // name code point
        if (is_ident_code_point(input)) {
            // Append the code point to result.
            result.append_code_point(input);
            continue;
        }

        // the stream starts with a valid escape
        if (is_valid_escape_sequence(start_of_input_stream_twin())) {
            // Consume an escaped code point. Append the returned code point to result.
            result.append_code_point(consume_escaped_code_point());
            continue;
        }

        // anything else
        // Reconsume the current input code point. Return result.
        reconsume_current_input_code_point();
        break;
    }

    return result.to_fly_string_without_validation();
}

// https://www.w3.org/TR/css-syntax-3/#consume-url-token
Token Tokenizer::consume_a_url_token()
{
    // This section describes how to consume a url token from a stream of code points.
    // It returns either a <url-token> or a <bad-url-token>.
    //
    // Note: This algorithm assumes that the initial "url(" has already been consumed.
    // This algorithm also assumes that it’s being called to consume an "unquoted" value,
    // like url(foo). A quoted value, like url("foo"), is parsed as a <function-token>.
    // Consume an ident-like token automatically handles this distinction; this algorithm
    // shouldn’t be called directly otherwise.

    // 1. Initially create a <url-token> with its value set to the empty string.
    auto start_byte_offset = current_byte_offset();
    auto token = create_new_token(Token::Type::Url);
    StringBuilder builder;

    // 2. Consume as much whitespace as possible.
    consume_as_much_whitespace_as_possible();

    auto make_token = [&]() -> Token {
        token.m_value = builder.to_fly_string_without_validation();
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    };

    // 3. Repeatedly consume the next input code point from the stream:
    for (;;) {
        auto input = next_code_point();

        // U+0029 RIGHT PARENTHESIS ())
        if (is_right_paren(input)) {
            // Return the <url-token>.
            return make_token();
        }

        // EOF
        if (is_eof(input)) {
            // This is a parse error. Return the <url-token>.
            log_parse_error();
            return make_token();
        }

        // whitespace
        if (is_whitespace(input)) {
            // Consume as much whitespace as possible.
            consume_as_much_whitespace_as_possible();

            // If the next input code point is U+0029 RIGHT PARENTHESIS ()) or EOF, consume it
            // and return the <url-token> (if EOF was encountered, this is a parse error);
            input = peek_code_point();

            if (is_right_paren(input)) {
                (void)next_code_point();
                return make_token();
            }

            if (is_eof(input)) {
                (void)next_code_point();
                log_parse_error();
                return make_token();
            }

            // otherwise, consume the remnants of a bad url, create a <bad-url-token>, and return it.
            consume_the_remnants_of_a_bad_url();
            auto bad_url_token = create_new_token(Token::Type::BadUrl);
            bad_url_token.m_original_source_text = input_since(start_byte_offset);
            return bad_url_token;
        }

        // U+0022 QUOTATION MARK (")
        // U+0027 APOSTROPHE (')
        // U+0028 LEFT PARENTHESIS (()
        // non-printable code point
        if (is_quotation_mark(input) || is_apostrophe(input) || is_left_paren(input) || is_non_printable(input)) {
            // This is a parse error. Consume the remnants of a bad url, create a <bad-url-token>, and return it.
            log_parse_error();
            consume_the_remnants_of_a_bad_url();
            auto bad_url_token = create_new_token(Token::Type::BadUrl);
            bad_url_token.m_original_source_text = input_since(start_byte_offset);
            return bad_url_token;
        }

        // U+005C REVERSE SOLIDUS (\)
        if (is_reverse_solidus(input)) {
            // If the stream starts with a valid escape,
            if (is_valid_escape_sequence(start_of_input_stream_twin())) {
                // consume an escaped code point and append the returned code point to the <url-token>’s value.
                builder.append_code_point(consume_escaped_code_point());
                continue;
            } else {
                // Otherwise, this is a parse error.
                log_parse_error();
                // Consume the remnants of a bad url, create a <bad-url-token>, and return it.
                consume_the_remnants_of_a_bad_url();
                auto bad_url_token = create_new_token(Token::Type::BadUrl);
                bad_url_token.m_original_source_text = input_since(start_byte_offset);
                return bad_url_token;
            }
        }

        // anything else
        // Append the current input code point to the <url-token>’s value.
        builder.append_code_point(input);
    }
}

// https://www.w3.org/TR/css-syntax-3/#consume-remnants-of-bad-url
void Tokenizer::consume_the_remnants_of_a_bad_url()
{
    // This section describes how to consume the remnants of a bad url from a stream of code points,
    // "cleaning up" after the tokenizer realizes that it’s in the middle of a <bad-url-token> rather
    // than a <url-token>. It returns nothing; its sole use is to consume enough of the input stream
    // to reach a recovery point where normal tokenizing can resume.

    // Repeatedly consume the next input code point from the stream:
    for (;;) {
        auto input = next_code_point();

        // U+0029 RIGHT PARENTHESIS ())
        // EOF
        if (is_eof(input) || is_right_paren(input)) {
            // Return.
            return;
        }

        // the input stream starts with a valid escape
        if (is_valid_escape_sequence(start_of_input_stream_twin())) {
            // Consume an escaped code point.
            // This allows an escaped right parenthesis ("\)") to be encountered without ending
            // the <bad-url-token>. This is otherwise identical to the "anything else" clause.
            (void)consume_escaped_code_point();
        }

        // anything else
        // Do nothing.
    }
}

void Tokenizer::consume_as_much_whitespace_as_possible()
{
    while (is_whitespace(peek_code_point())) {
        (void)next_code_point();
    }
}

void Tokenizer::reconsume_current_input_code_point()
{
    m_utf8_iterator = m_prev_utf8_iterator;
    m_position = m_prev_position;
}

// https://www.w3.org/TR/css-syntax-3/#consume-numeric-token
Token Tokenizer::consume_a_numeric_token()
{
    // This section describes how to consume a numeric token from a stream of code points.
    // It returns either a <number-token>, <percentage-token>, or <dimension-token>.

    auto start_byte_offset = current_byte_offset();

    // Consume a number and let number be the result.
    auto number = consume_a_number();

    // If the next 3 input code points would start an ident sequence, then:
    if (would_start_an_ident_sequence(peek_triplet())) {
        // 1. Create a <dimension-token> with the same value and type flag as number,
        //    and a unit set initially to the empty string.
        auto token = create_new_token(Token::Type::Dimension);
        token.m_number_value = number;

        // 2. Consume an ident sequence. Set the <dimension-token>’s unit to the returned value.
        auto unit = consume_an_ident_sequence();
        VERIFY(!unit.is_empty());
        // NOTE: We intentionally store this in the `value`, to save space.
        token.m_value = move(unit);

        // 3. Return the <dimension-token>.
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // Otherwise, if the next input code point is U+0025 PERCENTAGE SIGN (%), consume it.
    if (is_percent(peek_code_point())) {
        (void)next_code_point();

        // Create a <percentage-token> with the same value as number, and return it.
        auto token = create_new_token(Token::Type::Percentage);
        token.m_number_value = number;
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // Otherwise, create a <number-token> with the same value and type flag as number, and return it.
    auto token = create_new_token(Token::Type::Number);
    token.m_number_value = number;
    token.m_original_source_text = input_since(start_byte_offset);
    return token;
}

// https://www.w3.org/TR/css-syntax-3/#starts-with-a-number
bool Tokenizer::would_start_a_number(U32Triplet values)
{
    // This section describes how to check if three code points would start a number.
    // The algorithm described here can be called explicitly with three code points,
    // or can be called with the input stream itself. In the latter case, the three
    // code points in question are the current input code point and the next two input
    // code points, in that order.
    //
    // Note: This algorithm will not consume any additional code points.

    // Look at the first code point:

    // U+002B PLUS SIGN (+)
    // U+002D HYPHEN-MINUS (-)
    if (is_plus_sign(values.first) || is_hyphen_minus(values.first)) {
        // If the second code point is a digit, return true.
        if (is_ascii_digit(values.second))
            return true;

        // Otherwise, if the second code point is a U+002E FULL STOP (.) and the third
        // code point is a digit, return true.
        if (is_full_stop(values.second) && is_ascii_digit(values.third))
            return true;

        // Otherwise, return false.
        return false;
    }

    // U+002E FULL STOP (.)
    if (is_full_stop(values.first))
        // If the second code point is a digit, return true. Otherwise, return false.
        return is_ascii_digit(values.second);

    // digit
    if (is_ascii_digit(values.first))
        // Return true.
        return true;

    // anything else
    // Return false.
    return false;
}

// https://www.w3.org/TR/css-syntax-3/#starts-with-a-valid-escape
bool Tokenizer::is_valid_escape_sequence(U32Twin values)
{
    // This section describes how to check if two code points are a valid escape.
    // The algorithm described here can be called explicitly with two code points,
    // or can be called with the input stream itself. In the latter case, the two
    // code points in question are the current input code point and the next input
    // code point, in that order.
    //
    // Note: This algorithm will not consume any additional code point.

    // If the first code point is not U+005C REVERSE SOLIDUS (\), return false.
    if (!is_reverse_solidus(values.first))
        return false;

    // Otherwise, if the second code point is a newline, return false.
    if (is_newline(values.second))
        return false;

    // Otherwise, return true.
    return true;
}

// https://www.w3.org/TR/css-syntax-3/#would-start-an-identifier
bool Tokenizer::would_start_an_ident_sequence(U32Triplet values)
{
    // This section describes how to check if three code points would start an ident sequence.
    // The algorithm described here can be called explicitly with three code points, or
    // can be called with the input stream itself. In the latter case, the three code
    // points in question are the current input code point and the next two input code
    // points, in that order.
    //
    // Note: This algorithm will not consume any additional code points.

    // Look at the first code point:

    // U+002D HYPHEN-MINUS
    if (is_hyphen_minus(values.first)) {
        // If the second code point is a name-start code point or a U+002D HYPHEN-MINUS,
        // or the second and third code points are a valid escape, return true.
        if (is_ident_start_code_point(values.second) || is_hyphen_minus(values.second) || is_valid_escape_sequence(values.to_twin_23()))
            return true;
        // Otherwise, return false.
        return false;
    }

    // name-start code point
    if (is_ident_start_code_point(values.first)) {
        // Return true.
        return true;
    }

    // U+005C REVERSE SOLIDUS (\)
    if (is_reverse_solidus(values.first)) {
        // If the first and second code points are a valid escape, return true.
        if (is_valid_escape_sequence(values.to_twin_12()))
            return true;
        // Otherwise, return false.
        return false;
    }

    // anything else
    // Return false.
    return false;
}

// https://www.w3.org/TR/css-syntax-3/#consume-string-token
Token Tokenizer::consume_string_token(u32 ending_code_point)
{
    // This section describes how to consume a string token from a stream of code points.
    // It returns either a <string-token> or <bad-string-token>.
    //
    // This algorithm may be called with an ending code point, which denotes the code point
    // that ends the string. If an ending code point is not specified, the current input
    // code point is used.

    // Initially create a <string-token> with its value set to the empty string.
    auto start_byte_offset = current_byte_offset();
    auto token = create_new_token(Token::Type::String);
    StringBuilder builder;

    auto make_token = [&]() -> Token {
        token.m_value = builder.to_fly_string_without_validation();
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    };

    // Repeatedly consume the next input code point from the stream:
    for (;;) {
        auto input = next_code_point();

        // ending code point
        if (input == ending_code_point)
            return make_token();

        // EOF
        if (is_eof(input)) {
            // This is a parse error. Return the <string-token>.
            log_parse_error();
            return make_token();
        }

        // newline
        if (is_newline(input)) {
            // This is a parse error. Reconsume the current input code point, create a
            // <bad-string-token>, and return it.
            reconsume_current_input_code_point();
            auto bad_string_token = create_new_token(Token::Type::BadString);
            bad_string_token.m_original_source_text = input_since(start_byte_offset);
            return bad_string_token;
        }

        // U+005C REVERSE SOLIDUS (\)
        if (is_reverse_solidus(input)) {
            // If the next input code point is EOF, do nothing.
            auto next_input = peek_code_point();
            if (is_eof(next_input))
                continue;

            // Otherwise, if the next input code point is a newline, consume it.
            if (is_newline(next_input)) {
                (void)next_code_point();
                continue;
            }

            // Otherwise, (the stream starts with a valid escape) consume an escaped code
            // point and append the returned code point to the <string-token>’s value.
            auto escaped = consume_escaped_code_point();
            builder.append_code_point(escaped);
            continue;
        }

        // anything else
        // Append the current input code point to the <string-token>’s value.
        builder.append_code_point(input);
    }
}

// https://www.w3.org/TR/css-syntax-3/#consume-comment
void Tokenizer::consume_comments()
{
    // This section describes how to consume comments from a stream of code points.
    // It returns nothing.

start:
    // If the next two input code point are U+002F SOLIDUS (/) followed by a U+002A ASTERISK (*),
    // consume them and all following code points up to and including the first U+002A ASTERISK (*)
    // followed by a U+002F SOLIDUS (/), or up to an EOF code point. Return to the start of this step.
    //
    // If the preceding paragraph ended by consuming an EOF code point, this is a parse error.
    //
    // Return nothing.
    auto twin = peek_twin();
    if (!(is_solidus(twin.first) && is_asterisk(twin.second)))
        return;

    (void)next_code_point();
    (void)next_code_point();

    for (;;) {
        auto twin_inner = peek_twin();
        if (is_eof(twin_inner.first) || is_eof(twin_inner.second)) {
            log_parse_error();
            return;
        }

        if (is_asterisk(twin_inner.first) && is_solidus(twin_inner.second)) {
            (void)next_code_point();
            (void)next_code_point();
            goto start;
        }

        (void)next_code_point();
    }
}

// https://www.w3.org/TR/css-syntax-3/#consume-token
Token Tokenizer::consume_a_token()
{
    // This section describes how to consume a token from a stream of code points.
    // It will return a single token of any type.

    auto start_byte_offset = current_byte_offset();

    // Consume comments.
    consume_comments();

    // AD-HOC: Preserve comments as whitespace tokens, for serializing custom properties.
    auto after_comments_byte_offset = current_byte_offset();
    if (after_comments_byte_offset != start_byte_offset) {
        auto token = create_new_token(Token::Type::Whitespace);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // Consume the next input code point.
    auto input = next_code_point();

    // whitespace
    if (is_whitespace(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is whitespace");
        // Consume as much whitespace as possible. Return a <whitespace-token>.
        consume_as_much_whitespace_as_possible();
        auto token = create_new_token(Token::Type::Whitespace);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // U+0022 QUOTATION MARK (")
    if (is_quotation_mark(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is quotation mark");
        // Consume a string token and return it.
        return consume_string_token(input);
    }

    // U+0023 NUMBER SIGN (#)
    if (is_number_sign(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is number sign");

        // If the next input code point is an ident code point or the next two input code points
        // are a valid escape, then:
        auto next_input = peek_code_point();
        auto maybe_escape = peek_twin();

        if (is_ident_code_point(next_input) || is_valid_escape_sequence(maybe_escape)) {
            // 1. Create a <hash-token>.
            auto token = create_new_token(Token::Type::Hash);

            // 2. If the next 3 input code points would start an ident sequence, set the <hash-token>’s
            //    type flag to "id".
            if (would_start_an_ident_sequence(peek_triplet()))
                token.m_hash_type = Token::HashType::Id;

            // 3. Consume an ident sequence, and set the <hash-token>’s value to the returned string.
            auto name = consume_an_ident_sequence();
            token.m_value = move(name);

            // 4. Return the <hash-token>.
            token.m_original_source_text = input_since(start_byte_offset);
            return token;
        }

        // Otherwise, return a <delim-token> with its value set to the current input code point.
        return create_value_token(Token::Type::Delim, input, input_since(start_byte_offset));
    }

    // U+0027 APOSTROPHE (')
    if (is_apostrophe(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is apostrophe");
        // Consume a string token and return it.
        return consume_string_token(input);
    }

    // U+0028 LEFT PARENTHESIS (()
    if (is_left_paren(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is left paren");
        // Return a <(-token>.
        Token token = create_new_token(Token::Type::OpenParen);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // U+0029 RIGHT PARENTHESIS ())
    if (is_right_paren(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is right paren");
        // Return a <)-token>.
        Token token = create_new_token(Token::Type::CloseParen);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // U+002B PLUS SIGN (+)
    if (is_plus_sign(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is plus sign");
        // If the input stream starts with a number, reconsume the current input code point,
        // consume a numeric token and return it.
        if (would_start_a_number(start_of_input_stream_triplet())) {
            reconsume_current_input_code_point();
            return consume_a_numeric_token();
        }

        // Otherwise, return a <delim-token> with its value set to the current input code point.
        return create_value_token(Token::Type::Delim, input, input_since(start_byte_offset));
    }

    // U+002C COMMA (,)
    if (is_comma(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is comma");
        // Return a <comma-token>.
        Token token = create_new_token(Token::Type::Comma);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // U+002D HYPHEN-MINUS (-)
    if (is_hyphen_minus(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is hyphen minus");
        // If the input stream starts with a number, reconsume the current input code point,
        // consume a numeric token, and return it.
        if (would_start_a_number(start_of_input_stream_triplet())) {
            reconsume_current_input_code_point();
            return consume_a_numeric_token();
        }

        // Otherwise, if the next 2 input code points are U+002D HYPHEN-MINUS U+003E
        // GREATER-THAN SIGN (->), consume them and return a <CDC-token>.
        auto next_twin = peek_twin();
        if (is_hyphen_minus(next_twin.first) && is_greater_than_sign(next_twin.second)) {
            (void)next_code_point();
            (void)next_code_point();

            Token token = create_new_token(Token::Type::CDC);
            token.m_original_source_text = input_since(start_byte_offset);
            return token;
        }

        // Otherwise, if the input stream starts with an identifier, reconsume the current
        // input code point, consume an ident-like token, and return it.
        if (would_start_an_ident_sequence(start_of_input_stream_triplet())) {
            reconsume_current_input_code_point();
            return consume_an_ident_like_token();
        }

        // Otherwise, return a <delim-token> with its value set to the current input code point.
        return create_value_token(Token::Type::Delim, input, input_since(start_byte_offset));
    }

    // U+002E FULL STOP (.)
    if (is_full_stop(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is full stop");
        // If the input stream starts with a number, reconsume the current input code point,
        // consume a numeric token, and return it.
        if (would_start_a_number(start_of_input_stream_triplet())) {
            reconsume_current_input_code_point();
            return consume_a_numeric_token();
        }

        // Otherwise, return a <delim-token> with its value set to the current input code point.
        return create_value_token(Token::Type::Delim, input, input_since(start_byte_offset));
    }

    // U+003A COLON (:)
    if (is_colon(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is colon");
        // Return a <colon-token>.
        Token token = create_new_token(Token::Type::Colon);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // U+003B SEMICOLON (;)
    if (is_semicolon(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is semicolon");
        // Return a <semicolon-token>.
        Token token = create_new_token(Token::Type::Semicolon);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // U+003C LESS-THAN SIGN (<)
    if (is_less_than_sign(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is less than");
        // If the next 3 input code points are U+0021 EXCLAMATION MARK U+002D HYPHEN-MINUS
        // U+002D HYPHEN-MINUS (!--), consume them and return a <CDO-token>.
        auto maybe_cdo = peek_triplet();
        if (is_exclamation_mark(maybe_cdo.first) && is_hyphen_minus(maybe_cdo.second) && is_hyphen_minus(maybe_cdo.third)) {
            (void)next_code_point();
            (void)next_code_point();
            (void)next_code_point();

            Token token = create_new_token(Token::Type::CDO);
            token.m_original_source_text = input_since(start_byte_offset);
            return token;
        }

        // Otherwise, return a <delim-token> with its value set to the current input code point.
        return create_value_token(Token::Type::Delim, input, input_since(start_byte_offset));
    }

    // U+0040 COMMERCIAL AT (@)
    if (is_at(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is at");
        // If the next 3 input code points would start an ident sequence, consume an ident sequence, create
        // an <at-keyword-token> with its value set to the returned value, and return it.
        if (would_start_an_ident_sequence(peek_triplet())) {
            auto name = consume_an_ident_sequence();
            return create_value_token(Token::Type::AtKeyword, move(name), input_since(start_byte_offset));
        }

        // Otherwise, return a <delim-token> with its value set to the current input code point.
        return create_value_token(Token::Type::Delim, input, input_since(start_byte_offset));
    }

    // U+005B LEFT SQUARE BRACKET ([)
    if (is_open_square_bracket(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is open square");
        // Return a <[-token>.
        Token token = create_new_token(Token::Type::OpenSquare);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // U+005C REVERSE SOLIDUS (\)
    if (is_reverse_solidus(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is reverse solidus");
        // If the input stream starts with a valid escape, reconsume the current input code point,
        // consume an ident-like token, and return it.
        if (is_valid_escape_sequence(start_of_input_stream_twin())) {
            reconsume_current_input_code_point();
            return consume_an_ident_like_token();
        }

        // Otherwise, this is a parse error. Return a <delim-token> with its value set to the
        // current input code point.
        log_parse_error();
        return create_value_token(Token::Type::Delim, input, input_since(start_byte_offset));
    }

    // U+005D RIGHT SQUARE BRACKET (])
    if (is_closed_square_bracket(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is closed square");
        // Return a <]-token>.
        Token token = create_new_token(Token::Type::CloseSquare);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // U+007B LEFT CURLY BRACKET ({)
    if (is_open_curly_bracket(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is open curly");
        // Return a <{-token>.
        Token token = create_new_token(Token::Type::OpenCurly);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // U+007D RIGHT CURLY BRACKET (})
    if (is_closed_curly_bracket(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is closed curly");
        // Return a <}-token>.
        Token token = create_new_token(Token::Type::CloseCurly);
        token.m_original_source_text = input_since(start_byte_offset);
        return token;
    }

    // digit
    if (is_ascii_digit(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is digit");
        // Reconsume the current input code point, consume a numeric token, and return it.
        reconsume_current_input_code_point();
        return consume_a_numeric_token();
    }

    // name-start code point
    if (is_ident_start_code_point(input)) {
        dbgln_if(CSS_TOKENIZER_DEBUG, "is name start");
        // Reconsume the current input code point, consume an ident-like token, and return it.
        reconsume_current_input_code_point();
        return consume_an_ident_like_token();
    }

    // EOF
    if (is_eof(input)) {
        // Return an <EOF-token>.
        return create_eof_token();
    }

    // anything else
    dbgln_if(CSS_TOKENIZER_DEBUG, "is delimiter");
    // Return a <delim-token> with its value set to the current input code point.
    return create_value_token(Token::Type::Delim, input, input_since(start_byte_offset));
}

size_t Tokenizer::current_byte_offset() const
{
    return m_utf8_iterator.ptr() - m_utf8_view.bytes();
}

String Tokenizer::input_since(size_t offset) const
{
    return MUST(m_decoded_input.substring_from_byte_offset_with_shared_superstring(offset, current_byte_offset() - offset));
}

}

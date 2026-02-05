/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibURL/Pattern/Tokenizer.h>
#include <LibUnicode/CharacterTypes.h>

namespace URL::Pattern {

StringView Token::type_to_string(Token::Type type)
{
    switch (type) {
    case Token::Type::Open:
        return "Open"sv;
    case Token::Type::Close:
        return "Close"sv;
    case Token::Type::Regexp:
        return "Regexp"sv;
    case Token::Type::Name:
        return "Name"sv;
    case Token::Type::Char:
        return "Char"sv;
    case Token::Type::EscapedChar:
        return "EscapedChar"sv;
    case Token::Type::OtherModifier:
        return "OtherModifier"sv;
    case Token::Type::Asterisk:
        return "Asterisk"sv;
    case Token::Type::End:
        return "End"sv;
    case Token::Type::InvalidChar:
        return "InvalidChar"sv;
    }
    VERIFY_NOT_REACHED();
}

String Token::to_string() const
{
    return MUST(String::formatted("{}, index: {}, value: '{}'", type_to_string(type), index, value));
}

Tokenizer::Tokenizer(Utf8View const& input, Policy policy)
    : m_input(input)
    , m_policy(policy)
{
}

// https://urlpattern.spec.whatwg.org/#tokenize
PatternErrorOr<Vector<Token>> Tokenizer::tokenize(Utf8View const& input, Tokenizer::Policy policy)
{
    dbgln_if(URL_PATTERN_DEBUG, "URLPattern tokenizing input: '{}'", input.as_string());
    VERIFY(input.validate());

    // 1. Let tokenizer be a new tokenizer.
    // 2. Set tokenizer’s input to input.
    // 3. Set tokenizer’s policy to policy.
    Tokenizer tokenizer { input, policy };

    // 4. While tokenizer’s index is less than tokenizer’s input's code point length:
    while (tokenizer.m_index < tokenizer.m_input.length()) {
        // 1. Run seek and get the next code point given tokenizer and tokenizer’s index.
        tokenizer.seek_and_get_the_next_code_point(tokenizer.m_index);

        // 2. If tokenizer’s code point is U+002A (*):
        if (tokenizer.m_code_point == '*') {
            // 1. Run add a token with default position and length given tokenizer and "asterisk".
            tokenizer.add_a_token_with_default_position_and_length(Token::Type::Asterisk);

            // 2. Continue.
            continue;
        }

        // 3. If tokenizer’s code point is U+002B (+) or U+003F (?):
        if (tokenizer.m_code_point == '+' || tokenizer.m_code_point == '?') {
            // 1. Run add a token with default position and length given tokenizer and "other-modifier".
            tokenizer.add_a_token_with_default_position_and_length(Token::Type::OtherModifier);

            // 2. Continue.
            continue;
        }

        // 4. If tokenizer’s code point is U+005C (\):
        if (tokenizer.m_code_point == '\\') {
            // 1. If tokenizer’s index is equal to tokenizer’s input's code point length − 1:
            if (tokenizer.m_index == tokenizer.m_input.length() - 1) {
                // 1. Run process a tokenizing error given tokenizer, tokenizer’s next index, and tokenizer’s index.
                TRY(tokenizer.process_a_tokenizing_error(tokenizer.m_next_index, tokenizer.m_index));

                // 2. Continue.
                continue;
            }

            // 2. Let escaped index be tokenizer’s next index.
            auto escaped_index = tokenizer.m_next_index;

            // 3. Run get the next code point given tokenizer.
            tokenizer.get_the_next_code_point();

            // 4. Run add a token with default length given tokenizer, "escaped-char", tokenizer’s next index, and escaped index.
            tokenizer.add_a_token_with_default_length(Token::Type::EscapedChar, tokenizer.m_next_index, escaped_index);

            // 5. Continue.
            continue;
        }

        // 5. If tokenizer’s code point is U+007B ({):
        if (tokenizer.m_code_point == '{') {
            // 1. Run add a token with default position and length given tokenizer and "open".
            tokenizer.add_a_token_with_default_position_and_length(Token::Type::Open);

            // 2. Continue.
            continue;
        }

        // 6. If tokenizer’s code point is U+007D (}):
        if (tokenizer.m_code_point == '}') {
            // 1. Run add a token with default position and length given tokenizer and "close".
            tokenizer.add_a_token_with_default_position_and_length(Token::Type::Close);

            // 2. Continue.
            continue;
        }

        // 1. If tokenizer’s code point is U+003A (:):
        if (tokenizer.m_code_point == ':') {
            // 1. Let name position be tokenizer’s next index.
            auto name_position = tokenizer.m_next_index;

            // 2. Let name start be name position.
            auto name_start = name_position;

            // 3. While name position is less than tokenizer’s input's code point length:
            while (name_position < tokenizer.m_input.length()) {
                // 1. Run seek and get the next code point given tokenizer and name position.
                tokenizer.seek_and_get_the_next_code_point(name_position);

                // 2. Let first code point be true if name position equals name start and false otherwise.
                bool first_code_point = name_position == name_start;

                // 3. Let valid code point be the result of running is a valid name code point given tokenizer’s code point and first code point.
                bool valid_code_point = is_a_valid_name_code_point(tokenizer.m_code_point, first_code_point);

                // 4. If valid code point is false break.
                if (!valid_code_point)
                    break;

                // 5. Set name position to tokenizer’s next index.
                name_position = tokenizer.m_next_index;
            }

            // 4. If name position is less than or equal to name start:
            if (name_position <= name_start) {
                // 1. Run process a tokenizing error given tokenizer, name start, and tokenizer’s index.
                TRY(tokenizer.process_a_tokenizing_error(name_start, tokenizer.m_index));

                // 2. Continue.
                continue;
            }

            // 5. Run add a token with default length given tokenizer, "name", name position, and name start.
            tokenizer.add_a_token_with_default_length(Token::Type::Name, name_position, name_start);

            // 6. Continue.
            continue;
        }

        // 8. If tokenizer’s code point is U+0028 (():
        if (tokenizer.m_code_point == '(') {
            // 1. Let depth be 1.
            u32 depth = 1;

            // 2. Let regexp position be tokenizer’s next index.
            auto regexp_position = tokenizer.m_next_index;

            // 3. Let regexp start be regexp position.
            auto regexp_start = regexp_position;

            // 4. Let error be false.
            bool error = false;

            // 5. While regexp position is less than tokenizer’s input's code point length:
            while (regexp_position < tokenizer.m_input.length()) {
                // 1. Run seek and get the next code point given tokenizer and regexp position.
                tokenizer.seek_and_get_the_next_code_point(regexp_position);

                // 2. If the result of running is ASCII given tokenizer’s code point is false:
                if (!is_ascii(tokenizer.m_code_point)) {
                    // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                    TRY(tokenizer.process_a_tokenizing_error(regexp_start, tokenizer.m_index));

                    // 2. Set error to true.
                    error = true;

                    // 3. Break.
                    break;
                }

                // 3. If regexp position equals regexp start and tokenizer’s code point is U+003F (?):
                if (regexp_position == regexp_start && tokenizer.m_code_point == '?') {
                    // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                    TRY(tokenizer.process_a_tokenizing_error(regexp_start, tokenizer.m_index));

                    // 2. Set error to true.
                    error = true;

                    // 3. Break.
                    break;
                }

                // 4. If tokenizer’s code point is U+005C (\):
                if (tokenizer.m_code_point == '\\') {
                    // 1. If regexp position equals tokenizer’s input's code point length − 1:
                    if (regexp_position == tokenizer.m_input.length() - 1) {
                        // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                        TRY(tokenizer.process_a_tokenizing_error(regexp_start, tokenizer.m_index));

                        // 2. Set error to true.
                        error = true;

                        // 3. Break
                        break;
                    }

                    // 2. Run get the next code point given tokenizer.
                    tokenizer.get_the_next_code_point();

                    // 3. If the result of running is ASCII given tokenizer’s code point is false:
                    if (!is_ascii(tokenizer.m_code_point)) {
                        // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                        TRY(tokenizer.process_a_tokenizing_error(regexp_start, tokenizer.m_index));

                        // 2. Set error to true.
                        error = true;

                        // 3. Break.
                        break;
                    }

                    // 4. Set regexp position to tokenizer’s next index.
                    regexp_position = tokenizer.m_next_index;

                    // 5. Continue.
                    continue;
                }

                // 5. If tokenizer’s code point is U+0029 ()):
                if (tokenizer.m_code_point == ')') {
                    // 1. Decrement depth by 1.
                    --depth;

                    // 1. If depth is 0:
                    if (depth == 0) {
                        // 1. Set regexp position to tokenizer’s next index.
                        regexp_position = tokenizer.m_next_index;

                        // 2. Break.
                        break;
                    }
                }
                // 6. Otherwise if tokenizer’s code point is U+0028 (():
                else if (tokenizer.m_code_point == '(') {
                    // 1. Increment depth by 1.
                    ++depth;

                    // 2. If regexp position equals tokenizer’s input's code point length − 1:
                    if (regexp_position == tokenizer.m_input.length() - 1) {
                        // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                        TRY(tokenizer.process_a_tokenizing_error(regexp_start, tokenizer.m_index));

                        // 2. Set error to true.
                        error = true;

                        // 3. Break
                        break;
                    }

                    // 3. Let temporary position be tokenizer’s next index.
                    auto temporary_position = tokenizer.m_next_index;

                    // 4. Run get the next code point given tokenizer.
                    tokenizer.get_the_next_code_point();

                    // 5. If tokenizer’s code point is not U+003F (?):
                    if (tokenizer.m_code_point != '?') {
                        // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                        TRY(tokenizer.process_a_tokenizing_error(regexp_start, tokenizer.m_index));

                        // 2. Set error to true.
                        error = true;

                        // 3. Break.
                        break;
                    }

                    // 6. Set tokenizer’s next index to temporary position.
                    tokenizer.m_next_index = temporary_position;
                }

                // 7. Set regexp position to tokenizer’s next index.
                regexp_position = tokenizer.m_next_index;
            }

            // 6. If error is true continue.
            if (error)
                continue;

            // 7. If depth is not zero:
            if (depth != 0) {
                // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                TRY(tokenizer.process_a_tokenizing_error(regexp_start, tokenizer.m_index));

                // 2. Continue.
                continue;
            }

            // 8. Let regexp length be regexp position − regexp start − 1.
            auto regexp_length = regexp_position - regexp_start - 1;

            // 9. If regexp length is zero:
            if (regexp_length == 0) {
                // 1. Run process a tokenizing error given tokenizer, regexp start, and tokenizer’s index.
                TRY(tokenizer.process_a_tokenizing_error(regexp_start, tokenizer.m_index));

                // 2. Continue.
                continue;
            }

            // 10. Run add a token given tokenizer, "regexp", regexp position, regexp start, and regexp length.
            tokenizer.add_a_token(Token::Type::Regexp, regexp_position, regexp_start, regexp_length);

            // 11. Continue.
            continue;
        }

        // 9. Run add a token with default position and length given tokenizer and "char".
        tokenizer.add_a_token_with_default_position_and_length(Token::Type::Char);
    }

    // 5. Run add a token with default length given tokenizer, "end", tokenizer’s index, and tokenizer’s index.
    tokenizer.add_a_token_with_default_length(Token::Type::End, tokenizer.m_index, tokenizer.m_index);

    // 6. Return tokenizer’s token list.
    if constexpr (URL_PATTERN_DEBUG) {
        for (auto const& token : tokenizer.m_token_list)
            dbgln("{}", token.to_string());
    }

    return tokenizer.m_token_list;
}

// https://urlpattern.spec.whatwg.org/#get-the-next-code-point
void Tokenizer::get_the_next_code_point()
{
    // 1. Set tokenizer’s code point to the Unicode code point in tokenizer’s input at the position indicated by tokenizer’s next index.
    m_code_point = *m_input.unicode_substring_view(m_next_index, 1).begin();

    // 2. Increment tokenizer’s next index by 1.
    ++m_next_index;
}

// https://urlpattern.spec.whatwg.org/#seek-and-get-the-next-code-point
void Tokenizer::seek_and_get_the_next_code_point(u32 index)
{
    // 1. Set tokenizer’s next index to index.
    m_next_index = index;

    // 2. Run get the next code point given tokenizer.
    get_the_next_code_point();
}

// https://urlpattern.spec.whatwg.org/#add-a-token
void Tokenizer::add_a_token(Token::Type type, u32 next_position, u32 value_position, u32 value_length)
{
    // 1. Let token be a new token.
    Token token;

    // 2. Set token’s type to type.
    token.type = type;

    // 3. Set token’s index to tokenizer’s index.
    token.index = m_index;

    // 4. Set token’s value to the code point substring from value position with length value length within tokenizer’s input.
    token.value = MUST(String::from_utf8(m_input.unicode_substring_view(value_position, value_length).as_string()));

    // 5. Append token to the back of tokenizer’s token list.
    m_token_list.append(move(token));

    // 5. Set tokenizer’s index to next position.
    m_index = next_position;
}

// https://urlpattern.spec.whatwg.org/#add-a-token-with-default-length
void Tokenizer::add_a_token_with_default_length(Token::Type type, u32 next_position, u32 value_position)
{
    // 1. Let computed length be next position − value position.
    auto computed_length = next_position - value_position;

    // 2. Run add a token given tokenizer, type, next position, value position, and computed length.
    add_a_token(type, next_position, value_position, computed_length);
}

// https://urlpattern.spec.whatwg.org/#add-a-token-with-default-position-and-length
void Tokenizer::add_a_token_with_default_position_and_length(Token::Type type)
{
    // 1. Run add a token with default length given tokenizer, type, tokenizer’s next index, and tokenizer’s index.
    add_a_token_with_default_length(type, m_next_index, m_index);
}

// https://urlpattern.spec.whatwg.org/#process-a-tokenizing-error
PatternErrorOr<void> Tokenizer::process_a_tokenizing_error(u32 next_position, u32 value_position)
{
    // 1. If tokenizer’s policy is "strict", then throw a TypeError.
    if (m_policy == Policy::Strict)
        return ErrorInfo { "Error processing a token"_string }; // FIXME: Improve this error!

    // 2. Assert: tokenizer’s policy is "lenient".
    VERIFY(m_policy == Policy::Lenient);

    // 3. Run add a token with default length given tokenizer, "invalid-char", next position, and value position.
    add_a_token_with_default_length(Token::Type::InvalidChar, next_position, value_position);

    return {};
}

// https://urlpattern.spec.whatwg.org/#is-a-valid-name-code-point
bool Tokenizer::is_a_valid_name_code_point(u32 code_point, bool first)
{
    // 1. If first is true return the result of checking if code point is contained in the IdentifierStart set of code points.
    if (first)
        return code_point == '$' || code_point == '_' || Unicode::code_point_has_identifier_start_property(code_point);

    // 2. Otherwise return the result of checking if code point is contained in the IdentifierPart set of code points.
    return code_point == '$' || Unicode::code_point_has_identifier_continue_property(code_point);
}

}

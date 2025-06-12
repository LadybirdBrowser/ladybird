/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Pattern/Component.h>
#include <LibURL/Pattern/PatternParser.h>
#include <LibURL/Pattern/String.h>

namespace URL::Pattern {

PatternParser::PatternParser(EncodingCallback encoding_callback, String segment_wildcard_regexp)
    : m_encoding_callback(move(encoding_callback))
    , m_segment_wildcard_regexp(move(segment_wildcard_regexp))
{
}

// https://urlpattern.spec.whatwg.org/#consume-a-required-token
PatternErrorOr<void> PatternParser::consume_a_required_token(Token::Type type)
{
    // 1. Let result be the result of running try to consume a token given parser and type.
    auto result = try_to_consume_a_token(type);

    // 2. If result is null, then throw a TypeError.
    if (!result.has_value())
        return ErrorInfo { MUST(String::formatted("Missing required token '{}' in URL pattern", Token::type_to_string(type))) };

    // 3. Return result.
    // NOTE: No caller actually needs the result, so we just ignore it.
    return {};
}

// https://urlpattern.spec.whatwg.org/#consume-text
String PatternParser::consume_text()
{
    // 1. Let result be the empty string.
    StringBuilder result;

    // 1. While true:
    while (true) {
        // 1. Let token be the result of running try to consume a token given parser and "char".
        auto token = try_to_consume_a_token(Token::Type::Char);

        // 2. If token is null, then set token to the result of running try to consume a token given parser and "escaped-char".
        if (!token.has_value())
            token = try_to_consume_a_token(Token::Type::EscapedChar);

        // 3. If token is null, then break.
        if (!token.has_value())
            break;

        // 4. Append token’s value to the end of result.
        result.append(token->value);
    }

    // 2. Return result.
    return result.to_string_without_validation();
}

// https://urlpattern.spec.whatwg.org/#maybe-add-a-part-from-the-pending-fixed-value
PatternErrorOr<void> PatternParser::maybe_add_a_part_from_the_pending_fixed_value()
{
    // 1. If parser’s pending fixed value is the empty string, then return.
    if (m_pending_fixed_value.is_empty())
        return {};

    // 2. Let encoded value be the result of running parser’s encoding callback given parser’s pending fixed value.
    auto encoded_value = TRY(m_encoding_callback(m_pending_fixed_value.to_string_without_validation()));

    // 3. Set parser’s pending fixed value to the empty string.
    m_pending_fixed_value.clear();

    // 4. Let part be a new part whose type is "fixed-text", value is encoded value, and modifier is "none".
    // 5. Append part to parser’s part list.
    m_part_list.append({ Part::Type::FixedText, move(encoded_value), Part::Modifier::None });

    return {};
}

// https://urlpattern.spec.whatwg.org/#is-a-duplicate-name
bool PatternParser::is_a_duplicate_name(String const& name) const
{
    // 1. For each part of parser’s part list:
    for (auto const& part : m_part_list) {
        // 1. If part’s name is name, then return true.
        if (part.name == name)
            return true;
    }

    // 2. Return false.
    return false;
}

// https://urlpattern.spec.whatwg.org/#add-a-part
PatternErrorOr<void> PatternParser::add_a_part(String const& prefix, Optional<Token const&> name_token,
    Optional<Token const&> regexp_or_wildcard_token, String const& suffix, Optional<Token const&> modifier_token)
{
    // 1. Let modifier be "none".
    auto modifier = Part::Modifier::None;

    // 2. If modifier token is not null:
    if (modifier_token.has_value()) {
        // 1. If modifier token’s value is "?" then set modifier to "optional".
        if (modifier_token->value == "?"sv) {
            modifier = Part::Modifier::Optional;
        }
        // 2. Otherwise if modifier token’s value is "*" then set modifier to "zero-or-more".
        else if (modifier_token->value == "*"sv) {
            modifier = Part::Modifier::ZeroOrMore;
        }
        // 3. Otherwise if modifier token’s value is "+" then set modifier to "one-or-more".
        else if (modifier_token->value == "+"sv) {
            modifier = Part::Modifier::OneOrMore;
        }
    }

    // 3. If name token is null and regexp or wildcard token is null and modifier is "none":
    // NOTE: This was a "{foo}" grouping. We add this to the pending fixed value so that it will be combined with
    //       any previous or subsequent text.
    if (!name_token.has_value() && !regexp_or_wildcard_token.has_value() && modifier == Part::Modifier::None) {
        // 1. Append prefix to the end of parser’s pending fixed value.
        m_pending_fixed_value.append(prefix);

        // 2. Return.
        return {};
    }

    // 4. Run maybe add a part from the pending fixed value given parser.
    TRY(maybe_add_a_part_from_the_pending_fixed_value());

    // 5. If name token is null and regexp or wildcard token is null:
    // NOTE: This was a "{foo}?" grouping. The modifier means we cannot combine it with other text. Therefore we
    //       add it as a part immediately.
    if (!name_token.has_value() && !regexp_or_wildcard_token.has_value()) {
        // 1. Assert: suffix is the empty string.
        VERIFY(suffix.is_empty());

        // 2. If prefix is the empty string, then return.
        if (prefix.is_empty())
            return {};

        // 3. Let encoded value be the result of running parser’s encoding callback given prefix.
        auto encoded_value = TRY(m_encoding_callback(prefix));

        // 4. Let part be a new part whose type is "fixed-text", value is encoded value, and modifier is modifier.
        // 5. Append part to parser’s part list.
        m_part_list.append({ Part::Type::FixedText, move(encoded_value), modifier });

        // 6. Return.
        return {};
    }

    // 6. Let regexp value be the empty string.
    // NOTE: Next, we convert the regexp or wildcard token into a regular expression.
    String regexp_value;

    // 7. If regexp or wildcard token is null, then set regexp value to parser’s segment wildcard regexp.
    if (!regexp_or_wildcard_token.has_value()) {
        regexp_value = m_segment_wildcard_regexp;
    }
    // 8. Otherwise if regexp or wildcard token’s type is "asterisk", then set regexp value to the full wildcard regexp value.
    else if (regexp_or_wildcard_token->type == Token::Type::Asterisk) {
        regexp_value = MUST(String::from_utf8(full_wildcard_regexp_value));
    }
    // 9. Otherwise set regexp value to regexp or wildcard token’s value.
    else {
        regexp_value = regexp_or_wildcard_token->value;
    }

    // 10. Let type be "regexp".
    // NOTE: Next, we convert regexp value into a part type. We make sure to go to a regular expression first so
    //       that an equivalent "regexp" token will be treated the same as a "name" or "asterisk" token.
    auto type = Part::Type::Regexp;

    // 11. If regexp value is parser’s segment wildcard regexp:
    if (regexp_value == m_segment_wildcard_regexp) {
        // 1. Set type to "segment-wildcard".
        type = Part::Type::SegmentWildcard;

        // 2. Set regexp value to the empty string.
        regexp_value = String {};
    }
    // 12. Otherwise if regexp value is the full wildcard regexp value:
    else if (regexp_value == full_wildcard_regexp_value) {
        // 1. Set type to "full-wildcard".
        type = Part::Type::FullWildcard;

        // 2. Set regexp value to the empty string.
        regexp_value = String {};
    }

    // 13. Let name be the empty string.
    // NOTE: Next, we determine the part name. This can be explicitly provided by a "name" token or be automatically assigned.
    String name;

    // 14. If name token is not null, then set name to name token’s value.
    if (name_token.has_value()) {
        name = name_token->value;
    }
    // 15. Otherwise if regexp or wildcard token is not null:
    else if (regexp_or_wildcard_token.has_value()) {
        // 1. Set name to parser’s next numeric name, serialized.
        name = String::number(m_next_numeric_name);

        // 2. Increment parser’s next numeric name by 1.
        ++m_next_numeric_name;
    }

    // 16. If the result of running is a duplicate name given parser and name is true, then throw a TypeError.
    if (is_a_duplicate_name(name))
        return ErrorInfo { MUST(String::formatted("Duplicate name '{}' provided in URL pattern", name)) };

    // 17. Let encoded prefix be the result of running parser’s encoding callback given prefix.
    // NOTE: Finally, we encode the fixed text values and create the part.
    auto encoded_prefix = TRY(m_encoding_callback(prefix));

    // 18. Let encoded suffix be the result of running parser’s encoding callback given suffix.
    auto encoded_suffix = TRY(m_encoding_callback(suffix));

    // 19. Let part be a new part whose type is type, value is regexp value, modifier is modifier, name is name, prefix
    //     is encoded prefix, and suffix is encoded suffix.
    // 20. Append part to parser’s part list.
    m_part_list.append({ type, move(regexp_value), modifier, move(name), move(encoded_prefix), move(encoded_suffix) });

    return {};
}

// https://urlpattern.spec.whatwg.org/#try-to-consume-a-modifier-token
Optional<Token const&> PatternParser::try_to_consume_a_modifier_token()
{
    // 1. Let token be the result of running try to consume a token given parser and "other-modifier".
    auto token = try_to_consume_a_token(Token::Type::OtherModifier);

    // 2. If token is not null, then return token.
    if (token.has_value())
        return token;

    // 3. Set token to the result of running try to consume a token given parser and "asterisk".
    token = try_to_consume_a_token(Token::Type::Asterisk);

    // 4. Return token.
    return token;
}

// https://urlpattern.spec.whatwg.org/#try-to-consume-a-regexp-or-wildcard-token
Optional<Token const&> PatternParser::try_to_consume_a_regexp_or_wildcard_token(Optional<Token const&> name_token)
{
    // 1. Let token be the result of running try to consume a token given parser and "regexp".
    auto token = try_to_consume_a_token(Token::Type::Regexp);

    // 2. If name token is null and token is null, then set token to the result of running try to consume a token given
    //    parser and "asterisk".
    if (!name_token.has_value() && !token.has_value())
        token = try_to_consume_a_token(Token::Type::Asterisk);

    // 3. Return token.
    return token;
}

// https://urlpattern.spec.whatwg.org/#try-to-consume-a-token
Optional<Token const&> PatternParser::try_to_consume_a_token(Token::Type type)
{
    // 1. Assert: parser’s index is less than parser’s token list size.
    VERIFY(m_index < m_token_list.size());

    // 2. Let next token be parser’s token list[parser’s index].
    auto const& next_token = m_token_list[m_index];

    // 3. If next token’s type is not type return null.
    if (next_token.type != type)
        return {};

    // 4. Increment parser’s index by 1.
    ++m_index;

    // 5. Return next token.
    return next_token;
}

// https://urlpattern.spec.whatwg.org/#parse-a-pattern-string
PatternErrorOr<Vector<Part>> PatternParser::parse(Utf8View const& input, Options const& options, EncodingCallback encoding_callback)
{
    // 1. Let parser be a new pattern parser whose encoding callback is encoding callback and segment wildcard regexp
    //    is the result of running generate a segment wildcard regexp given options.
    PatternParser parser { move(encoding_callback), generate_a_segment_wildcard_regexp(options) };

    // 2. Set parser’s token list to the result of running tokenize given input and "strict".
    parser.m_token_list = TRY(Tokenizer::tokenize(input, Tokenizer::Policy::Strict));

    // 3. While parser’s index is less than parser’s token list's size:
    while (parser.m_index < parser.m_token_list.size()) {
        // 1. Let char token be the result of running try to consume a token given parser and "char".
        auto char_token = parser.try_to_consume_a_token(Token::Type::Char);

        // 2. Let name token be the result of running try to consume a token given parser and "name".
        auto name_token = parser.try_to_consume_a_token(Token::Type::Name);

        // 3. Let regexp or wildcard token be the result of running try to consume a regexp or wildcard token given
        //    parser and name token.
        auto regexp_or_wildcard_token = parser.try_to_consume_a_regexp_or_wildcard_token(name_token);

        // 4. If name token is not null or regexp or wildcard token is not null:
        // NOTE: If there is a matching group, we need to add the part immediately.
        if (name_token.has_value() || regexp_or_wildcard_token.has_value()) {
            // 1. Let prefix be the empty string.
            String prefix;

            // 2. If char token is not null then set prefix to char token’s value.
            if (char_token.has_value())
                prefix = char_token->value;

            // 3. If prefix is not the empty string and not options’s prefix code point:
            if (!prefix.is_empty() && (!options.prefix_code_point.has_value() || prefix != String::from_code_point(*options.prefix_code_point))) {
                // 1. Append prefix to the end of parser’s pending fixed value.
                parser.m_pending_fixed_value.append(prefix);

                // 2. Set prefix to the empty string.
                prefix = String {};
            }

            // 4. Run maybe add a part from the pending fixed value given parser.
            TRY(parser.maybe_add_a_part_from_the_pending_fixed_value());

            // 5. Let modifier token be the result of running try to consume a modifier token given parser.
            auto modifier_token = parser.try_to_consume_a_modifier_token();

            // 6. Run add a part given parser, prefix, name token, regexp or wildcard token, the empty string,
            //    and modifier token.
            TRY(parser.add_a_part(prefix, name_token, regexp_or_wildcard_token, String {}, modifier_token));

            // 7. Continue.
            continue;
        }

        // 5. Let fixed token be char token.
        // NOTE: If there was no matching group, then we need to buffer any fixed text. We want to collect as
        //       much text as possible before adding it as a "fixed-text" part.
        auto fixed_token = char_token;

        // 6. If fixed token is null, then set fixed token to the result of running try to consume a token given
        //     parser and "escaped-char".
        if (!fixed_token.has_value())
            fixed_token = parser.try_to_consume_a_token(Token::Type::EscapedChar);

        // 7. If fixed token is not null:
        if (fixed_token.has_value()) {
            // 1. Append fixed token’s value to parser’s pending fixed value.
            parser.m_pending_fixed_value.append(fixed_token->value);

            // 2. Continue.
            continue;
        }

        // 8. Let open token be the result of running try to consume a token given parser and "open".
        auto open_token = parser.try_to_consume_a_token(Token::Type::Open);

        // 9. If open token is not null:
        if (open_token.has_value()) {
            // 1. Let prefix be the result of running consume text given parser.
            auto prefix = parser.consume_text();

            // 2. Set name token to the result of running try to consume a token given parser and "name".
            name_token = parser.try_to_consume_a_token(Token::Type::Name);

            // 3. Set regexp or wildcard token to the result of running try to consume a regexp or wildcard token
            //    given parser and name token.
            regexp_or_wildcard_token = parser.try_to_consume_a_regexp_or_wildcard_token(name_token);

            // 4. Let suffix be the result of running consume text given parser.
            auto suffix = parser.consume_text();

            // 5. Run consume a required token given parser and "close".
            TRY(parser.consume_a_required_token(Token::Type::Close));

            // 6. Let modifier token to the result of running try to consume a modifier token given parser.
            auto modifier_token = parser.try_to_consume_a_modifier_token();

            // 7. Run add a part given parser, prefix, name token, regexp or wildcard token, suffix, and modifier token.
            TRY(parser.add_a_part(prefix, name_token, regexp_or_wildcard_token, suffix, modifier_token));

            // 8. Continue.
            continue;
        }

        // 10. Run maybe add a part from the pending fixed value given parser.
        TRY(parser.maybe_add_a_part_from_the_pending_fixed_value());

        // 11. Run consume a required token given parser and "end".
        TRY(parser.consume_a_required_token(Token::Type::End));
    }

    if constexpr (URL_PATTERN_DEBUG) {
        dbgln("Pattern parser produced the part list:");
        for (auto const& part : parser.m_part_list) {
            dbgln("Type {}, Value '{}', Modifier {}, Name '{}', Prefix '{}', Suffix '{}'",
                Part::type_to_string(part.type), part.value, Part::convert_modifier_to_string(part.modifier),
                part.name, part.prefix, part.suffix);
        }
    }
    // 4. Return parser’s part list.
    return move(parser.m_part_list);
}

}

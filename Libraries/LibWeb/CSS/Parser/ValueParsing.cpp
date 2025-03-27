/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tommy van der Vorst <tommy@pixelspark.nl>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/GenericLexer.h>
#include <AK/TemporaryChange.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundRepeatStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSColor.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSHSL.h>
#include <LibWeb/CSS/StyleValues/CSSHWB.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/CSSLCHLike.h>
#include <LibWeb/CSS/StyleValues/CSSLabLike.h>
#include <LibWeb/CSS/StyleValues/CSSLightDark.h>
#include <LibWeb/CSS/StyleValues/CSSRGB.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/FitContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/LinearGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::CSS::Parser {

Optional<Dimension> Parser::parse_dimension(ComponentValue const& component_value)
{
    if (component_value.is(Token::Type::Dimension)) {
        auto numeric_value = component_value.token().dimension_value();
        auto unit_string = component_value.token().dimension_unit();

        if (auto length_type = Length::unit_from_name(unit_string); length_type.has_value())
            return Length { numeric_value, length_type.release_value() };

        if (auto angle_type = Angle::unit_from_name(unit_string); angle_type.has_value())
            return Angle { numeric_value, angle_type.release_value() };

        if (auto flex_type = Flex::unit_from_name(unit_string); flex_type.has_value())
            return Flex { numeric_value, flex_type.release_value() };

        if (auto frequency_type = Frequency::unit_from_name(unit_string); frequency_type.has_value())
            return Frequency { numeric_value, frequency_type.release_value() };

        if (auto resolution_type = Resolution::unit_from_name(unit_string); resolution_type.has_value())
            return Resolution { numeric_value, resolution_type.release_value() };

        if (auto time_type = Time::unit_from_name(unit_string); time_type.has_value())
            return Time { numeric_value, time_type.release_value() };
    }

    if (component_value.is(Token::Type::Percentage))
        return Percentage { component_value.token().percentage() };

    if (component_value.is(Token::Type::Number)) {
        auto numeric_value = component_value.token().number_value();
        if (numeric_value == 0)
            return Length::make_px(0);

        if (context_allows_quirky_length())
            return Length::make_px(CSSPixels::nearest_value_for(numeric_value));
    }

    return {};
}

Optional<AngleOrCalculated> Parser::parse_angle(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_angle_value(tokens)) {
        if (value->is_angle())
            return value->as_angle().angle();
        if (value->is_calculated())
            return AngleOrCalculated { value->as_calculated() };
    }
    return {};
}

Optional<AnglePercentage> Parser::parse_angle_percentage(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_angle_percentage_value(tokens)) {
        if (value->is_angle())
            return value->as_angle().angle();
        if (value->is_percentage())
            return value->as_percentage().percentage();
        if (value->is_calculated())
            return AnglePercentage { value->as_calculated() };
    }
    return {};
}

Optional<FlexOrCalculated> Parser::parse_flex(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_flex_value(tokens)) {
        if (value->is_flex())
            return value->as_flex().flex();
        if (value->is_calculated())
            return FlexOrCalculated { value->as_calculated() };
    }
    return {};
}

Optional<FrequencyOrCalculated> Parser::parse_frequency(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_frequency_value(tokens)) {
        if (value->is_frequency())
            return value->as_frequency().frequency();
        if (value->is_calculated())
            return FrequencyOrCalculated { value->as_calculated() };
    }
    return {};
}

Optional<FrequencyPercentage> Parser::parse_frequency_percentage(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_frequency_percentage_value(tokens)) {
        if (value->is_frequency())
            return value->as_frequency().frequency();
        if (value->is_percentage())
            return value->as_percentage().percentage();
        if (value->is_calculated())
            return FrequencyPercentage { value->as_calculated() };
    }
    return {};
}

Optional<IntegerOrCalculated> Parser::parse_integer(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_integer_value(tokens)) {
        if (value->is_integer())
            return value->as_integer().integer();
        if (value->is_calculated())
            return IntegerOrCalculated { value->as_calculated() };
    }
    return {};
}

Optional<LengthOrCalculated> Parser::parse_length(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_length_value(tokens)) {
        if (value->is_length())
            return value->as_length().length();
        if (value->is_calculated())
            return LengthOrCalculated { value->as_calculated() };
    }
    return {};
}

Optional<LengthPercentage> Parser::parse_length_percentage(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_length_percentage_value(tokens)) {
        if (value->is_length())
            return value->as_length().length();
        if (value->is_percentage())
            return value->as_percentage().percentage();
        if (value->is_calculated())
            return LengthPercentage { value->as_calculated() };
    }
    return {};
}

Optional<NumberOrCalculated> Parser::parse_number(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_number_value(tokens)) {
        if (value->is_number())
            return value->as_number().number();
        if (value->is_calculated())
            return NumberOrCalculated { value->as_calculated() };
    }
    return {};
}

Optional<NumberPercentage> Parser::parse_number_percentage(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_number_percentage_value(tokens)) {
        if (value->is_number())
            return Number { Number::Type::Number, value->as_number().number() };
        if (value->is_percentage())
            return value->as_percentage().percentage();
        if (value->is_calculated())
            return NumberPercentage { value->as_calculated() };
    }
    return {};
}

Optional<ResolutionOrCalculated> Parser::parse_resolution(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_resolution_value(tokens)) {
        if (value->is_resolution())
            return value->as_resolution().resolution();
        if (value->is_calculated())
            return ResolutionOrCalculated { value->as_calculated() };
    }
    return {};
}

Optional<TimeOrCalculated> Parser::parse_time(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_time_value(tokens)) {
        if (value->is_time())
            return value->as_time().time();
        if (value->is_calculated())
            return TimeOrCalculated { value->as_calculated() };
    }
    return {};
}

Optional<TimePercentage> Parser::parse_time_percentage(TokenStream<ComponentValue>& tokens)
{
    if (auto value = parse_time_percentage_value(tokens)) {
        if (value->is_time())
            return value->as_time().time();
        if (value->is_percentage())
            return value->as_percentage().percentage();
        if (value->is_calculated())
            return TimePercentage { value->as_calculated() };
    }
    return {};
}

Optional<Ratio> Parser::parse_ratio(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    // FIXME: It seems like `calc(...) / calc(...)` is a valid <ratio>, but this case is neither mentioned in a spec,
    //        nor tested in WPT, as far as I can tell.
    //        Still, we should probably support it. That means not assuming we can resolve the calculation immediately.

    auto read_number_value = [this](ComponentValue const& component_value) -> Optional<double> {
        if (component_value.is(Token::Type::Number))
            return component_value.token().number_value();

        if (component_value.is_function()) {
            auto maybe_calc = parse_calculated_value(component_value);
            if (!maybe_calc)
                return {};
            if (maybe_calc->is_number())
                return maybe_calc->as_number().value();
            if (!maybe_calc->is_calculated() || !maybe_calc->as_calculated().resolves_to_number())
                return {};
            if (auto resolved_number = maybe_calc->as_calculated().resolve_number({}); resolved_number.has_value() && resolved_number.value() >= 0) {
                return resolved_number.value();
            }
        }
        return {};
    };

    // `<ratio> = <number [0,∞]> [ / <number [0,∞]> ]?`
    auto maybe_numerator = read_number_value(tokens.consume_a_token());
    if (!maybe_numerator.has_value() || maybe_numerator.value() < 0)
        return {};
    auto numerator = maybe_numerator.value();

    {
        auto two_value_transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        auto const& solidus = tokens.consume_a_token();
        tokens.discard_whitespace();
        auto maybe_denominator = read_number_value(tokens.consume_a_token());

        if (solidus.is_delim('/') && maybe_denominator.has_value() && maybe_denominator.value() >= 0) {
            auto denominator = maybe_denominator.value();
            // Two-value ratio
            two_value_transaction.commit();
            transaction.commit();
            return Ratio { numerator, denominator };
        }
    }

    // Single-value ratio
    transaction.commit();
    return Ratio { numerator };
}

// https://drafts.csswg.org/css-fonts-4/#family-name-syntax
RefPtr<CSSStyleValue> Parser::parse_family_name_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    // <family-name> = <string> | <custom-ident>+
    Vector<String> parts;
    while (tokens.has_next_token()) {
        auto const& peek = tokens.next_token();

        if (peek.is(Token::Type::String)) {
            // `font-family: my cool "font";` is invalid.
            if (!parts.is_empty())
                return nullptr;
            tokens.discard_a_token(); // String
            tokens.discard_whitespace();
            transaction.commit();
            return StringStyleValue::create(peek.token().string());
        }

        if (peek.is(Token::Type::Ident)) {
            auto ident = tokens.consume_a_token().token().ident();

            // CSS-wide keywords are not allowed
            if (is_css_wide_keyword(ident))
                return nullptr;

            // <generic-family> is a separate type from <family-name>, and so isn't allowed here.
            auto maybe_keyword = keyword_from_string(ident);
            if (maybe_keyword.has_value() && keyword_to_generic_font_family(maybe_keyword.value()).has_value()) {
                return nullptr;
            }

            parts.append(ident.to_string());
            tokens.discard_whitespace();
            continue;
        }

        break;
    }

    if (parts.is_empty())
        return nullptr;

    transaction.commit();
    return CustomIdentStyleValue::create(MUST(String::join(' ', parts)));
}

// https://www.w3.org/TR/css-syntax-3/#urange-syntax
Optional<Gfx::UnicodeRange> Parser::parse_unicode_range(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    // <urange> =
    //  u '+' <ident-token> '?'* |
    //  u <dimension-token> '?'* |
    //  u <number-token> '?'* |
    //  u <number-token> <dimension-token> |
    //  u <number-token> <number-token> |
    //  u '+' '?'+
    // (All with no whitespace in between tokens.)

    // NOTE: Parsing this is different from usual. We take these steps:
    // 1. Match the grammar above against the tokens, concatenating them into a string using their original representation.
    // 2. Then, parse that string according to the spec algorithm.
    // Step 2 is performed by calling the other parse_unicode_range() overload.

    auto is_ending_token = [](ComponentValue const& component_value) {
        return component_value.is(Token::Type::EndOfFile)
            || component_value.is(Token::Type::Comma)
            || component_value.is(Token::Type::Semicolon)
            || component_value.is(Token::Type::Whitespace);
    };

    auto create_unicode_range = [&](StringView text, auto& local_transaction) -> Optional<Gfx::UnicodeRange> {
        auto maybe_unicode_range = parse_unicode_range(text);
        if (maybe_unicode_range.has_value()) {
            local_transaction.commit();
            transaction.commit();
        }
        return maybe_unicode_range;
    };

    // All options start with 'u'/'U'.
    auto const& u = tokens.consume_a_token();
    if (!u.is_ident("u"sv)) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> does not start with 'u'");
        return {};
    }

    auto const& second_token = tokens.consume_a_token();

    //  u '+' <ident-token> '?'* |
    //  u '+' '?'+
    if (second_token.is_delim('+')) {
        auto local_transaction = tokens.begin_transaction();
        StringBuilder string_builder;
        string_builder.append(second_token.token().original_source_text());

        auto const& third_token = tokens.consume_a_token();
        if (third_token.is(Token::Type::Ident) || third_token.is_delim('?')) {
            string_builder.append(third_token.token().original_source_text());
            while (tokens.next_token().is_delim('?'))
                string_builder.append(tokens.consume_a_token().token().original_source_text());
            if (is_ending_token(tokens.next_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        }
    }

    //  u <dimension-token> '?'*
    if (second_token.is(Token::Type::Dimension)) {
        auto local_transaction = tokens.begin_transaction();
        StringBuilder string_builder;
        string_builder.append(second_token.token().original_source_text());
        while (tokens.next_token().is_delim('?'))
            string_builder.append(tokens.consume_a_token().token().original_source_text());
        if (is_ending_token(tokens.next_token()))
            return create_unicode_range(string_builder.string_view(), local_transaction);
    }

    //  u <number-token> '?'* |
    //  u <number-token> <dimension-token> |
    //  u <number-token> <number-token>
    if (second_token.is(Token::Type::Number)) {
        auto local_transaction = tokens.begin_transaction();
        StringBuilder string_builder;
        string_builder.append(second_token.token().original_source_text());

        if (is_ending_token(tokens.next_token()))
            return create_unicode_range(string_builder.string_view(), local_transaction);

        auto const& third_token = tokens.consume_a_token();
        if (third_token.is_delim('?')) {
            string_builder.append(third_token.token().original_source_text());
            while (tokens.next_token().is_delim('?'))
                string_builder.append(tokens.consume_a_token().token().original_source_text());
            if (is_ending_token(tokens.next_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        } else if (third_token.is(Token::Type::Dimension)) {
            string_builder.append(third_token.token().original_source_text());
            if (is_ending_token(tokens.next_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        } else if (third_token.is(Token::Type::Number)) {
            string_builder.append(third_token.token().original_source_text());
            if (is_ending_token(tokens.next_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        }
    }

    if constexpr (CSS_PARSER_DEBUG) {
        dbgln("CSSParser: Tokens did not match <urange> grammar.");
        tokens.dump_all_tokens();
    }
    return {};
}

Optional<Gfx::UnicodeRange> Parser::parse_unicode_range(StringView text)
{
    auto make_valid_unicode_range = [&](u32 start_value, u32 end_value) -> Optional<Gfx::UnicodeRange> {
        // https://www.w3.org/TR/css-syntax-3/#maximum-allowed-code-point
        constexpr u32 maximum_allowed_code_point = 0x10FFFF;

        // To determine what codepoints the <urange> represents:
        // 1. If end value is greater than the maximum allowed code point,
        //    the <urange> is invalid and a syntax error.
        if (end_value > maximum_allowed_code_point) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Invalid <urange>: end_value ({}) > maximum ({})", end_value, maximum_allowed_code_point);
            return {};
        }

        // 2. If start value is greater than end value, the <urange> is invalid and a syntax error.
        if (start_value > end_value) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Invalid <urange>: start_value ({}) > end_value ({})", start_value, end_value);
            return {};
        }

        // 3. Otherwise, the <urange> represents a contiguous range of codepoints from start value to end value, inclusive.
        return Gfx::UnicodeRange { start_value, end_value };
    };

    // 1. Skipping the first u token, concatenate the representations of all the tokens in the production together.
    //    Let this be text.
    // NOTE: The concatenation is already done by the caller.
    GenericLexer lexer { text };

    // 2. If the first character of text is U+002B PLUS SIGN, consume it.
    //    Otherwise, this is an invalid <urange>, and this algorithm must exit.
    if (lexer.next_is('+')) {
        lexer.consume();
    } else {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Second character of <urange> was not '+'; got: '{}'", lexer.consume());
        return {};
    }

    // 3. Consume as many hex digits from text as possible.
    //    then consume as many U+003F QUESTION MARK (?) code points as possible.
    auto start_position = lexer.tell();
    auto hex_digits = lexer.consume_while(is_ascii_hex_digit);
    auto question_marks = lexer.consume_while([](auto it) { return it == '?'; });
    //    If zero code points were consumed, or more than six code points were consumed,
    //    this is an invalid <urange>, and this algorithm must exit.
    size_t consumed_code_points = hex_digits.length() + question_marks.length();
    if (consumed_code_points == 0 || consumed_code_points > 6) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> start value had {} digits/?s, expected between 1 and 6.", consumed_code_points);
        return {};
    }
    StringView start_value_code_points = text.substring_view(start_position, consumed_code_points);

    //    If any U+003F QUESTION MARK (?) code points were consumed, then:
    if (question_marks.length() > 0) {
        // 1. If there are any code points left in text, this is an invalid <urange>,
        //    and this algorithm must exit.
        if (lexer.tell_remaining() != 0) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> invalid; had {} code points left over.", lexer.tell_remaining());
            return {};
        }

        // 2. Interpret the consumed code points as a hexadecimal number,
        //    with the U+003F QUESTION MARK (?) code points replaced by U+0030 DIGIT ZERO (0) code points.
        //    This is the start value.
        auto start_value_string = start_value_code_points.replace("?"sv, "0"sv, ReplaceMode::All);
        auto maybe_start_value = AK::StringUtils::convert_to_uint_from_hex<u32>(start_value_string);
        if (!maybe_start_value.has_value()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> ?-converted start value did not parse as hex number.");
            return {};
        }
        u32 start_value = maybe_start_value.release_value();

        // 3. Interpret the consumed code points as a hexadecimal number again,
        //    with the U+003F QUESTION MARK (?) code points replaced by U+0046 LATIN CAPITAL LETTER F (F) code points.
        //    This is the end value.
        auto end_value_string = start_value_code_points.replace("?"sv, "F"sv, ReplaceMode::All);
        auto maybe_end_value = AK::StringUtils::convert_to_uint_from_hex<u32>(end_value_string);
        if (!maybe_end_value.has_value()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> ?-converted end value did not parse as hex number.");
            return {};
        }
        u32 end_value = maybe_end_value.release_value();

        // 4. Exit this algorithm.
        return make_valid_unicode_range(start_value, end_value);
    }
    //   Otherwise, interpret the consumed code points as a hexadecimal number. This is the start value.
    auto maybe_start_value = AK::StringUtils::convert_to_uint_from_hex<u32>(start_value_code_points);
    if (!maybe_start_value.has_value()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> start value did not parse as hex number.");
        return {};
    }
    u32 start_value = maybe_start_value.release_value();

    // 4. If there are no code points left in text, The end value is the same as the start value.
    //    Exit this algorithm.
    if (lexer.tell_remaining() == 0)
        return make_valid_unicode_range(start_value, start_value);

    // 5. If the next code point in text is U+002D HYPHEN-MINUS (-), consume it.
    if (lexer.next_is('-')) {
        lexer.consume();
    }
    //    Otherwise, this is an invalid <urange>, and this algorithm must exit.
    else {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> start and end values not separated by '-'.");
        return {};
    }

    // 6. Consume as many hex digits as possible from text.
    auto end_hex_digits = lexer.consume_while(is_ascii_hex_digit);

    //   If zero hex digits were consumed, or more than 6 hex digits were consumed,
    //   this is an invalid <urange>, and this algorithm must exit.
    if (end_hex_digits.length() == 0 || end_hex_digits.length() > 6) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> end value had {} digits, expected between 1 and 6.", end_hex_digits.length());
        return {};
    }

    //   If there are any code points left in text, this is an invalid <urange>, and this algorithm must exit.
    if (lexer.tell_remaining() != 0) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> invalid; had {} code points left over.", lexer.tell_remaining());
        return {};
    }

    // 7. Interpret the consumed code points as a hexadecimal number. This is the end value.
    auto maybe_end_value = AK::StringUtils::convert_to_uint_from_hex<u32>(end_hex_digits);
    if (!maybe_end_value.has_value()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> end value did not parse as hex number.");
        return {};
    }
    u32 end_value = maybe_end_value.release_value();

    return make_valid_unicode_range(start_value, end_value);
}

Vector<Gfx::UnicodeRange> Parser::parse_unicode_ranges(TokenStream<ComponentValue>& tokens)
{
    Vector<Gfx::UnicodeRange> unicode_ranges;
    auto range_token_lists = parse_a_comma_separated_list_of_component_values(tokens);
    for (auto& range_tokens : range_token_lists) {
        TokenStream range_token_stream { range_tokens };
        auto maybe_unicode_range = parse_unicode_range(range_token_stream);
        if (!maybe_unicode_range.has_value()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: unicode-range format invalid; discarding.");
            return {};
        }
        unicode_ranges.append(maybe_unicode_range.release_value());
    }
    return unicode_ranges;
}

RefPtr<UnicodeRangeStyleValue> Parser::parse_unicode_range_value(TokenStream<ComponentValue>& tokens)
{
    if (auto range = parse_unicode_range(tokens); range.has_value())
        return UnicodeRangeStyleValue::create(range.release_value());
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_integer_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Number) && peek_token.token().number().is_integer()) {
        tokens.discard_a_token(); // integer
        return IntegerStyleValue::create(peek_token.token().number().integer_value());
    }
    if (auto calc = parse_calculated_value(peek_token); calc
        && (calc->is_integer() || (calc->is_calculated() && calc->as_calculated().resolves_to_number()))) {

        tokens.discard_a_token(); // calc
        return calc;
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_number_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Number)) {
        tokens.discard_a_token(); // number
        return NumberStyleValue::create(peek_token.token().number().value());
    }
    if (auto calc = parse_calculated_value(peek_token); calc
        && (calc->is_number() || (calc->is_calculated() && calc->as_calculated().resolves_to_number()))) {

        tokens.discard_a_token(); // calc
        return calc;
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_number_percentage_value(TokenStream<ComponentValue>& tokens)
{
    // Parses [<percentage> | <number>] (which is equivalent to [<alpha-value>])
    if (auto value = parse_number_value(tokens))
        return value;
    if (auto value = parse_percentage_value(tokens))
        return value;
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_number_percentage_none_value(TokenStream<ComponentValue>& tokens)
{
    // Parses [<percentage> | <number> | none] (which is equivalent to [<alpha-value> | none])
    if (auto value = parse_number_value(tokens))
        return value;
    if (auto value = parse_percentage_value(tokens))
        return value;

    if (tokens.next_token().is_ident("none"sv)) {
        tokens.discard_a_token(); // keyword none
        return CSSKeywordValue::create(Keyword::None);
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_percentage_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Percentage)) {
        tokens.discard_a_token(); // percentage
        return PercentageStyleValue::create(Percentage(peek_token.token().percentage()));
    }
    if (auto calc = parse_calculated_value(peek_token); calc
        && (calc->is_percentage() || (calc->is_calculated() && calc->as_calculated().resolves_to_percentage()))) {

        tokens.discard_a_token(); // calc
        return calc;
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_angle_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto angle_type = Angle::unit_from_name(dimension_token.dimension_unit()); angle_type.has_value()) {
            transaction.commit();
            return AngleStyleValue::create(Angle { (dimension_token.dimension_value()), angle_type.release_value() });
        }
        return nullptr;
    }

    // https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
    // When parsing an SVG attribute, an angle is allowed without a unit.
    // FIXME: How should these numbers be interpreted? https://github.com/w3c/svgwg/issues/792
    //        For now: Convert to an angle in degrees.
    if (tokens.next_token().is(Token::Type::Number) && is_parsing_svg_presentation_attribute()) {
        auto numeric_value = tokens.consume_a_token().token().number_value();
        return AngleStyleValue::create(Angle::make_degrees(numeric_value));
    }

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_angle() || (calc->is_calculated() && calc->as_calculated().resolves_to_angle()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_angle_percentage_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto angle_type = Angle::unit_from_name(dimension_token.dimension_unit()); angle_type.has_value()) {
            transaction.commit();
            return AngleStyleValue::create(Angle { (dimension_token.dimension_value()), angle_type.release_value() });
        }
        return nullptr;
    }

    if (tokens.next_token().is(Token::Type::Percentage))
        return PercentageStyleValue::create(Percentage { tokens.consume_a_token().token().percentage() });

    // https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
    // When parsing an SVG attribute, an angle is allowed without a unit.
    // FIXME: How should these numbers be interpreted? https://github.com/w3c/svgwg/issues/792
    //        For now: Convert to an angle in degrees.
    if (tokens.next_token().is(Token::Type::Number) && is_parsing_svg_presentation_attribute()) {
        auto numeric_value = tokens.consume_a_token().token().number_value();
        return AngleStyleValue::create(Angle::make_degrees(numeric_value));
    }

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_angle() || calc->is_percentage() || (calc->is_calculated() && calc->as_calculated().resolves_to_angle_percentage()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_flex_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto flex_type = Flex::unit_from_name(dimension_token.dimension_unit()); flex_type.has_value()) {
            transaction.commit();
            return FlexStyleValue::create(Flex { (dimension_token.dimension_value()), flex_type.release_value() });
        }
        return nullptr;
    }

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_flex() || (calc->is_calculated() && calc->as_calculated().resolves_to_flex()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_frequency_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto frequency_type = Frequency::unit_from_name(dimension_token.dimension_unit()); frequency_type.has_value()) {
            transaction.commit();
            return FrequencyStyleValue::create(Frequency { (dimension_token.dimension_value()), frequency_type.release_value() });
        }
        return nullptr;
    }

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_frequency() || (calc->is_calculated() && calc->as_calculated().resolves_to_frequency()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_frequency_percentage_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto frequency_type = Frequency::unit_from_name(dimension_token.dimension_unit()); frequency_type.has_value()) {
            transaction.commit();
            return FrequencyStyleValue::create(Frequency { (dimension_token.dimension_value()), frequency_type.release_value() });
        }
        return nullptr;
    }

    if (tokens.next_token().is(Token::Type::Percentage))
        return PercentageStyleValue::create(Percentage { tokens.consume_a_token().token().percentage() });

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_frequency() || calc->is_percentage() || (calc->is_calculated() && calc->as_calculated().resolves_to_frequency_percentage()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_length_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto length_type = Length::unit_from_name(dimension_token.dimension_unit()); length_type.has_value()) {
            transaction.commit();
            return LengthStyleValue::create(Length { (dimension_token.dimension_value()), length_type.release_value() });
        }
        return nullptr;
    }

    if (tokens.next_token().is(Token::Type::Number)) {
        auto transaction = tokens.begin_transaction();
        auto numeric_value = tokens.consume_a_token().token().number_value();
        if (numeric_value == 0) {
            transaction.commit();
            return LengthStyleValue::create(Length::make_px(0));
        }
        if (context_allows_quirky_length()) {
            transaction.commit();
            return LengthStyleValue::create(Length::make_px(CSSPixels::nearest_value_for(numeric_value)));
        }

        // https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
        // When parsing an SVG attribute, a length is allowed without a unit.
        // FIXME: How should these numbers be interpreted? https://github.com/w3c/svgwg/issues/792
        //        For now: Convert to a length in pixels.
        if (is_parsing_svg_presentation_attribute()) {
            transaction.commit();
            return LengthStyleValue::create(Length::make_px(CSSPixels::nearest_value_for(numeric_value)));
        }
    }

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_length() || (calc->is_calculated() && calc->as_calculated().resolves_to_length()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_length_percentage_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto length_type = Length::unit_from_name(dimension_token.dimension_unit()); length_type.has_value()) {
            transaction.commit();
            return LengthStyleValue::create(Length { (dimension_token.dimension_value()), length_type.release_value() });
        }
        return nullptr;
    }

    if (tokens.next_token().is(Token::Type::Percentage))
        return PercentageStyleValue::create(Percentage { tokens.consume_a_token().token().percentage() });

    if (tokens.next_token().is(Token::Type::Number)) {
        auto transaction = tokens.begin_transaction();
        auto numeric_value = tokens.consume_a_token().token().number_value();
        if (numeric_value == 0) {
            transaction.commit();
            return LengthStyleValue::create(Length::make_px(0));
        }
        if (context_allows_quirky_length()) {
            transaction.commit();
            return LengthStyleValue::create(Length::make_px(CSSPixels::nearest_value_for(numeric_value)));
        }

        // https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
        // When parsing an SVG attribute, a length is allowed without a unit.
        // FIXME: How should these numbers be interpreted? https://github.com/w3c/svgwg/issues/792
        //        For now: Convert to a length in pixels.
        if (is_parsing_svg_presentation_attribute()) {
            transaction.commit();
            return LengthStyleValue::create(Length::make_px(CSSPixels::nearest_value_for(numeric_value)));
        }
    }

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_length() || calc->is_percentage() || (calc->is_calculated() && calc->as_calculated().resolves_to_length_percentage()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_resolution_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto resolution_type = Resolution::unit_from_name(dimension_token.dimension_unit()); resolution_type.has_value()) {
            transaction.commit();
            return ResolutionStyleValue::create(Resolution { (dimension_token.dimension_value()), resolution_type.release_value() });
        }
        return nullptr;
    }

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_resolution() || (calc->is_calculated() && calc->as_calculated().resolves_to_resolution()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_time_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto time_type = Time::unit_from_name(dimension_token.dimension_unit()); time_type.has_value()) {
            transaction.commit();
            return TimeStyleValue::create(Time { (dimension_token.dimension_value()), time_type.release_value() });
        }
        return nullptr;
    }

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_time() || (calc->is_calculated() && calc->as_calculated().resolves_to_time()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_time_percentage_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto time_type = Time::unit_from_name(dimension_token.dimension_unit()); time_type.has_value()) {
            transaction.commit();
            return TimeStyleValue::create(Time { (dimension_token.dimension_value()), time_type.release_value() });
        }
        return nullptr;
    }

    if (tokens.next_token().is(Token::Type::Percentage))
        return PercentageStyleValue::create(Percentage { tokens.consume_a_token().token().percentage() });

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_time() || calc->is_percentage() || (calc->is_calculated() && calc->as_calculated().resolves_to_time_percentage()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_keyword_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Ident)) {
        auto keyword = keyword_from_string(peek_token.token().ident());
        if (keyword.has_value()) {
            tokens.discard_a_token(); // ident
            return CSSKeywordValue::create(keyword.value());
        }
    }

    return nullptr;
}

// https://www.w3.org/TR/CSS2/visufx.html#value-def-shape
RefPtr<CSSStyleValue> Parser::parse_rect_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto const& function_token = tokens.consume_a_token();
    if (!function_token.is_function("rect"sv))
        return nullptr;

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { "rect"sv });

    Vector<Length, 4> params;
    auto argument_tokens = TokenStream { function_token.function().value };

    enum class CommaRequirement {
        Unknown,
        RequiresCommas,
        RequiresNoCommas
    };

    enum class Side {
        Top = 0,
        Right = 1,
        Bottom = 2,
        Left = 3
    };

    auto comma_requirement = CommaRequirement::Unknown;

    // In CSS 2.1, the only valid <shape> value is: rect(<top>, <right>, <bottom>, <left>) where
    // <top> and <bottom> specify offsets from the top border edge of the box, and <right>, and
    //  <left> specify offsets from the left border edge of the box.
    for (size_t side = 0; side < 4; side++) {
        argument_tokens.discard_whitespace();

        // <top>, <right>, <bottom>, and <left> may either have a <length> value or 'auto'.
        // Negative lengths are permitted.
        if (argument_tokens.next_token().is_ident("auto"sv)) {
            (void)argument_tokens.consume_a_token(); // `auto`
            params.append(Length::make_auto());
        } else {
            auto maybe_length = parse_length(argument_tokens);
            if (!maybe_length.has_value())
                return nullptr;
            if (maybe_length.value().is_calculated()) {
                dbgln("FIXME: Support calculated lengths in rect(): {}", maybe_length.value().calculated()->to_string(CSS::CSSStyleValue::SerializationMode::Normal));
                return nullptr;
            }
            params.append(maybe_length.value().value());
        }
        argument_tokens.discard_whitespace();

        // The last side, should be no more tokens following it.
        if (static_cast<Side>(side) == Side::Left) {
            if (argument_tokens.has_next_token())
                return nullptr;
            break;
        }

        bool next_is_comma = argument_tokens.next_token().is(Token::Type::Comma);

        // Authors should separate offset values with commas. User agents must support separation
        // with commas, but may also support separation without commas (but not a combination),
        // because a previous revision of this specification was ambiguous in this respect.
        if (comma_requirement == CommaRequirement::Unknown)
            comma_requirement = next_is_comma ? CommaRequirement::RequiresCommas : CommaRequirement::RequiresNoCommas;

        if (comma_requirement == CommaRequirement::RequiresCommas) {
            if (next_is_comma)
                argument_tokens.discard_a_token();
            else
                return nullptr;
        } else if (comma_requirement == CommaRequirement::RequiresNoCommas) {
            if (next_is_comma)
                return nullptr;
        } else {
            VERIFY_NOT_REACHED();
        }
    }

    transaction.commit();
    return RectStyleValue::create(EdgeRect { params[0], params[1], params[2], params[3] });
}

// https://www.w3.org/TR/css-color-4/#typedef-hue
RefPtr<CSSStyleValue> Parser::parse_hue_none_value(TokenStream<ComponentValue>& tokens)
{
    // Parses [<hue> | none]
    //   <hue> = <number> | <angle>

    if (auto angle = parse_angle_value(tokens))
        return angle;
    if (auto number = parse_number_value(tokens))
        return number;
    if (tokens.next_token().is_ident("none"sv)) {
        tokens.discard_a_token(); // keyword none
        return CSSKeywordValue::create(Keyword::None);
    }

    return nullptr;
}

// https://www.w3.org/TR/css-color-4/#typedef-color-alpha-value
RefPtr<CSSStyleValue> Parser::parse_solidus_and_alpha_value(TokenStream<ComponentValue>& tokens)
{
    // [ / [<alpha-value> | none] ]?
    // <alpha-value> = <number> | <percentage>
    // Common to the modern-syntax color functions.

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    if (!tokens.consume_a_token().is_delim('/'))
        return {};
    tokens.discard_whitespace();
    auto alpha = parse_number_percentage_none_value(tokens);
    if (!alpha)
        return {};
    tokens.discard_whitespace();

    transaction.commit();
    return alpha;
}

// https://www.w3.org/TR/css-color-4/#funcdef-rgb
RefPtr<CSSStyleValue> Parser::parse_rgb_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // rgb() = [ <legacy-rgb-syntax> | <modern-rgb-syntax> ]
    // rgba() = [ <legacy-rgba-syntax> | <modern-rgba-syntax> ]
    // <legacy-rgb-syntax> = rgb( <percentage>#{3} , <alpha-value>? ) |
    //                       rgb( <number>#{3} , <alpha-value>? )
    // <legacy-rgba-syntax> = rgba( <percentage>#{3} , <alpha-value>? ) |
    //                        rgba( <number>#{3} , <alpha-value>? )
    // <modern-rgb-syntax> = rgb(
    //     [ <number> | <percentage> | none]{3}
    //     [ / [<alpha-value> | none] ]?  )
    // <modern-rgba-syntax> = rgba(
    //     [ <number> | <percentage> | none]{3}
    //     [ / [<alpha-value> | none] ]?  )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function("rgb"sv) && !function_token.is_function("rgba"sv))
        return {};

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.function().name });

    RefPtr<CSSStyleValue> red;
    RefPtr<CSSStyleValue> green;
    RefPtr<CSSStyleValue> blue;
    RefPtr<CSSStyleValue> alpha;

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    red = parse_number_percentage_none_value(inner_tokens);
    if (!red)
        return {};

    inner_tokens.discard_whitespace();
    bool legacy_syntax = inner_tokens.next_token().is(Token::Type::Comma);
    if (legacy_syntax) {
        // Legacy syntax
        //   <percentage>#{3} , <alpha-value>?
        //   | <number>#{3} , <alpha-value>?
        // So, r/g/b can be numbers or percentages, as long as they're all the same type.

        // We accepted the 'none' keyword when parsing the red value, but it's not allowed in the legacy syntax.
        if (red->is_keyword())
            return {};

        inner_tokens.discard_a_token(); // comma
        inner_tokens.discard_whitespace();

        green = parse_number_percentage_value(inner_tokens);
        if (!green)
            return {};
        inner_tokens.discard_whitespace();

        if (!inner_tokens.consume_a_token().is(Token::Type::Comma))
            return {};
        inner_tokens.discard_whitespace();

        blue = parse_number_percentage_value(inner_tokens);
        if (!blue)
            return {};
        inner_tokens.discard_whitespace();

        if (inner_tokens.has_next_token()) {
            // Try and read comma and alpha
            if (!inner_tokens.consume_a_token().is(Token::Type::Comma))
                return {};
            inner_tokens.discard_whitespace();

            alpha = parse_number_percentage_value(inner_tokens);

            if (!alpha)
                return {};

            inner_tokens.discard_whitespace();

            if (inner_tokens.has_next_token())
                return {};
        }

        // Verify we're all percentages or all numbers
        auto is_percentage = [](CSSStyleValue const& style_value) {
            return style_value.is_percentage()
                || (style_value.is_calculated() && style_value.as_calculated().resolves_to_percentage());
        };
        bool red_is_percentage = is_percentage(*red);
        bool green_is_percentage = is_percentage(*green);
        bool blue_is_percentage = is_percentage(*blue);
        if (red_is_percentage != green_is_percentage || red_is_percentage != blue_is_percentage)
            return {};

    } else {
        // Modern syntax
        //   [ <number> | <percentage> | none]{3}  [ / [<alpha-value> | none] ]?

        green = parse_number_percentage_none_value(inner_tokens);
        if (!green)
            return {};
        inner_tokens.discard_whitespace();

        blue = parse_number_percentage_none_value(inner_tokens);
        if (!blue)
            return {};
        inner_tokens.discard_whitespace();

        if (inner_tokens.has_next_token()) {
            alpha = parse_solidus_and_alpha_value(inner_tokens);
            if (!alpha || inner_tokens.has_next_token())
                return {};
        }
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();
    return CSSRGB::create(red.release_nonnull(), green.release_nonnull(), blue.release_nonnull(), alpha.release_nonnull(), legacy_syntax ? ColorSyntax::Legacy : ColorSyntax::Modern);
}

// https://www.w3.org/TR/css-color-4/#funcdef-hsl
RefPtr<CSSStyleValue> Parser::parse_hsl_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // hsl() = [ <legacy-hsl-syntax> | <modern-hsl-syntax> ]
    // hsla() = [ <legacy-hsla-syntax> | <modern-hsla-syntax> ]
    // <modern-hsl-syntax> = hsl(
    //     [<hue> | none]
    //     [<percentage> | <number> | none]
    //     [<percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )
    // <modern-hsla-syntax> = hsla(
    //     [<hue> | none]
    //     [<percentage> | <number> | none]
    //     [<percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )
    // <legacy-hsl-syntax> = hsl( <hue>, <percentage>, <percentage>, <alpha-value>? )
    // <legacy-hsla-syntax> = hsla( <hue>, <percentage>, <percentage>, <alpha-value>? )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function("hsl"sv) && !function_token.is_function("hsla"sv))
        return {};

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.function().name });

    RefPtr<CSSStyleValue> h;
    RefPtr<CSSStyleValue> s;
    RefPtr<CSSStyleValue> l;
    RefPtr<CSSStyleValue> alpha;

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    h = parse_hue_none_value(inner_tokens);
    if (!h)
        return {};

    inner_tokens.discard_whitespace();
    bool legacy_syntax = inner_tokens.next_token().is(Token::Type::Comma);
    if (legacy_syntax) {
        // Legacy syntax
        //   <hue>, <percentage>, <percentage>, <alpha-value>?

        // We accepted the 'none' keyword when parsing the h value, but it's not allowed in the legacy syntax.
        if (h->is_keyword())
            return {};

        (void)inner_tokens.consume_a_token(); // comma
        inner_tokens.discard_whitespace();

        s = parse_percentage_value(inner_tokens);
        if (!s)
            return {};
        inner_tokens.discard_whitespace();

        if (!inner_tokens.consume_a_token().is(Token::Type::Comma))
            return {};
        inner_tokens.discard_whitespace();

        l = parse_percentage_value(inner_tokens);
        if (!l)
            return {};
        inner_tokens.discard_whitespace();

        if (inner_tokens.has_next_token()) {
            // Try and read comma and alpha
            if (!inner_tokens.consume_a_token().is(Token::Type::Comma))
                return {};
            inner_tokens.discard_whitespace();

            alpha = parse_number_percentage_value(inner_tokens);
            // The parser has consumed a comma, so the alpha value is now required
            if (!alpha)
                return {};
            inner_tokens.discard_whitespace();

            if (inner_tokens.has_next_token())
                return {};
        }
    } else {
        // Modern syntax
        //   [<hue> | none]
        //   [<percentage> | <number> | none]
        //   [<percentage> | <number> | none]
        //   [ / [<alpha-value> | none] ]?

        s = parse_number_percentage_none_value(inner_tokens);
        if (!s)
            return {};
        inner_tokens.discard_whitespace();

        l = parse_number_percentage_none_value(inner_tokens);
        if (!l)
            return {};
        inner_tokens.discard_whitespace();

        if (inner_tokens.has_next_token()) {
            alpha = parse_solidus_and_alpha_value(inner_tokens);
            if (!alpha || inner_tokens.has_next_token())
                return {};
        }
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();
    return CSSHSL::create(h.release_nonnull(), s.release_nonnull(), l.release_nonnull(), alpha.release_nonnull(), legacy_syntax ? ColorSyntax::Legacy : ColorSyntax::Modern);
}

// https://www.w3.org/TR/css-color-4/#funcdef-hwb
RefPtr<CSSStyleValue> Parser::parse_hwb_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // hwb() = hwb(
    //     [<hue> | none]
    //     [<percentage> | <number> | none]
    //     [<percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function("hwb"sv))
        return {};

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.function().name });

    RefPtr<CSSStyleValue> h;
    RefPtr<CSSStyleValue> w;
    RefPtr<CSSStyleValue> b;
    RefPtr<CSSStyleValue> alpha;

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    h = parse_hue_none_value(inner_tokens);
    if (!h)
        return {};
    inner_tokens.discard_whitespace();

    w = parse_number_percentage_none_value(inner_tokens);
    if (!w)
        return {};
    inner_tokens.discard_whitespace();

    b = parse_number_percentage_none_value(inner_tokens);
    if (!b)
        return {};
    inner_tokens.discard_whitespace();

    if (inner_tokens.has_next_token()) {
        alpha = parse_solidus_and_alpha_value(inner_tokens);
        if (!alpha || inner_tokens.has_next_token())
            return {};
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();
    return CSSHWB::create(h.release_nonnull(), w.release_nonnull(), b.release_nonnull(), alpha.release_nonnull());
}

Optional<Array<RefPtr<CSSStyleValue>, 4>> Parser::parse_lab_like_color_value(TokenStream<ComponentValue>& outer_tokens, StringView function_name)
{
    // This helper is designed to be compatible with lab and oklab and parses a function with a form like:
    // f() = f( [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function(function_name))
        return OptionalNone {};

    RefPtr<CSSStyleValue> l;
    RefPtr<CSSStyleValue> a;
    RefPtr<CSSStyleValue> b;
    RefPtr<CSSStyleValue> alpha;

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    l = parse_number_percentage_none_value(inner_tokens);
    if (!l)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    a = parse_number_percentage_none_value(inner_tokens);
    if (!a)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    b = parse_number_percentage_none_value(inner_tokens);
    if (!b)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    if (inner_tokens.has_next_token()) {
        alpha = parse_solidus_and_alpha_value(inner_tokens);
        if (!alpha || inner_tokens.has_next_token())
            return OptionalNone {};
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();

    return Array { move(l), move(a), move(b), move(alpha) };
}

// https://www.w3.org/TR/css-color-4/#funcdef-lab
RefPtr<CSSStyleValue> Parser::parse_lab_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // lab() = lab( [<percentage> | <number> | none]
    //      [ <percentage> | <number> | none]
    //      [ <percentage> | <number> | none]
    //      [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lab_like_color_value(outer_tokens, "lab"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return CSSLabLike::create<CSSLab>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-oklab
RefPtr<CSSStyleValue> Parser::parse_oklab_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // oklab() = oklab( [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lab_like_color_value(outer_tokens, "oklab"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return CSSLabLike::create<CSSOKLab>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

Optional<Array<RefPtr<CSSStyleValue>, 4>> Parser::parse_lch_like_color_value(TokenStream<ComponentValue>& outer_tokens, StringView function_name)
{
    // This helper is designed to be compatible with lch and oklch and parses a function with a form like:
    // f() = f( [<percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <hue> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto const& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function(function_name))
        return OptionalNone {};

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    auto l = parse_number_percentage_none_value(inner_tokens);
    if (!l)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    auto c = parse_number_percentage_none_value(inner_tokens);
    if (!c)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    auto h = parse_hue_none_value(inner_tokens);
    if (!h)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    RefPtr<CSSStyleValue> alpha;
    if (inner_tokens.has_next_token()) {
        alpha = parse_solidus_and_alpha_value(inner_tokens);
        if (!alpha || inner_tokens.has_next_token())
            return OptionalNone {};
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();

    return Array { move(l), move(c), move(h), move(alpha) };
}

// https://www.w3.org/TR/css-color-4/#funcdef-lch
RefPtr<CSSStyleValue> Parser::parse_lch_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // lch() = lch( [<percentage> | <number> | none]
    //      [ <percentage> | <number> | none]
    //      [ <hue> | none]
    //      [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lch_like_color_value(outer_tokens, "lch"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return CSSLCHLike::create<CSSLCH>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-oklch
RefPtr<CSSStyleValue> Parser::parse_oklch_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // oklch() = oklch( [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <hue> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lch_like_color_value(outer_tokens, "oklch"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return CSSLCHLike::create<CSSOKLCH>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-color
RefPtr<CSSStyleValue> Parser::parse_color_function(TokenStream<ComponentValue>& outer_tokens)
{
    // color() = color( <colorspace-params> [ / [ <alpha-value> | none ] ]? )
    //     <colorspace-params> = [ <predefined-rgb-params> | <xyz-params>]
    //     <predefined-rgb-params> = <predefined-rgb> [ <number> | <percentage> | none ]{3}
    //     <predefined-rgb> = srgb | srgb-linear | display-p3 | a98-rgb | prophoto-rgb | rec2020
    //     <xyz-params> = <xyz-space> [ <number> | <percentage> | none ]{3}
    //     <xyz-space> = xyz | xyz-d50 | xyz-d65

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto const& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function("color"sv))
        return {};

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.function().name });

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    auto const& maybe_color_space = inner_tokens.consume_a_token();
    inner_tokens.discard_whitespace();
    if (!any_of(CSSColor::s_supported_color_space, [&](auto supported) { return maybe_color_space.is_ident(supported); }))
        return {};

    auto const& color_space = maybe_color_space.token().ident();

    auto c1 = parse_number_percentage_none_value(inner_tokens);
    if (!c1)
        return {};
    inner_tokens.discard_whitespace();

    auto c2 = parse_number_percentage_none_value(inner_tokens);
    if (!c2)
        return {};
    inner_tokens.discard_whitespace();

    auto c3 = parse_number_percentage_none_value(inner_tokens);
    if (!c3)
        return {};
    inner_tokens.discard_whitespace();

    RefPtr<CSSStyleValue> alpha;
    if (inner_tokens.has_next_token()) {
        alpha = parse_solidus_and_alpha_value(inner_tokens);
        if (!alpha || inner_tokens.has_next_token())
            return {};
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();
    return CSSColor::create(color_space.to_ascii_lowercase(),
        c1.release_nonnull(),
        c2.release_nonnull(),
        c3.release_nonnull(),
        alpha.release_nonnull());
}

// https://drafts.csswg.org/css-color-5/#funcdef-light-dark
RefPtr<CSSStyleValue> Parser::parse_light_dark_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    auto transaction = outer_tokens.begin_transaction();

    outer_tokens.discard_whitespace();
    auto const& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function("light-dark"sv))
        return {};

    auto inner_tokens = TokenStream { function_token.function().value };

    inner_tokens.discard_whitespace();
    auto light = parse_color_value(inner_tokens);
    if (!light)
        return {};

    inner_tokens.discard_whitespace();
    if (!inner_tokens.consume_a_token().is(Token::Type::Comma))
        return {};

    inner_tokens.discard_whitespace();
    auto dark = parse_color_value(inner_tokens);
    if (!dark)
        return {};

    inner_tokens.discard_whitespace();
    if (inner_tokens.has_next_token())
        return {};

    transaction.commit();
    return CSSLightDark::create(light.release_nonnull(), dark.release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#color-syntax
RefPtr<CSSStyleValue> Parser::parse_color_value(TokenStream<ComponentValue>& tokens)
{

    // Keywords: <system-color> | <deprecated-color> | currentColor
    {
        auto transaction = tokens.begin_transaction();
        if (auto keyword = parse_keyword_value(tokens); keyword && keyword->has_color()) {
            transaction.commit();
            return keyword;
        }
    }

    // Functions
    if (auto color = parse_color_function(tokens))
        return color;

    if (auto rgb = parse_rgb_color_value(tokens))
        return rgb;
    if (auto hsl = parse_hsl_color_value(tokens))
        return hsl;
    if (auto hwb = parse_hwb_color_value(tokens))
        return hwb;
    if (auto lab = parse_lab_color_value(tokens))
        return lab;
    if (auto lch = parse_lch_color_value(tokens))
        return lch;
    if (auto oklab = parse_oklab_color_value(tokens))
        return oklab;
    if (auto oklch = parse_oklch_color_value(tokens))
        return oklch;
    if (auto light_dark = parse_light_dark_color_value(tokens))
        return light_dark;

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& component_value = tokens.consume_a_token();

    if (component_value.is(Token::Type::Ident)) {
        auto ident = component_value.token().ident();

        auto color = Color::from_string(ident);
        if (color.has_value()) {
            transaction.commit();
            return CSSColorValue::create_from_color(color.release_value(), ColorSyntax::Legacy, ident);
        }
        // Otherwise, fall through to the hashless-hex-color case
    }

    if (component_value.is(Token::Type::Hash)) {
        auto color = Color::from_string(MUST(String::formatted("#{}", component_value.token().hash_value())));
        if (color.has_value()) {
            transaction.commit();
            return CSSColorValue::create_from_color(color.release_value(), ColorSyntax::Legacy);
        }
        return {};
    }

    // https://drafts.csswg.org/css-color-4/#quirky-color
    if (in_quirks_mode()) {
        // "When CSS is being parsed in quirks mode, <quirky-color> is a type of <color> that is only valid in certain properties:"
        // (NOTE: List skipped for brevity; quirks data is assigned in Properties.json)
        // "It is not valid in properties that include or reference these properties, such as the background shorthand,
        // or inside functional notations such as color-mix()"

        bool quirky_color_allowed = false;
        if (!m_value_context.is_empty()) {
            quirky_color_allowed = m_value_context.first().visit(
                [](PropertyID const& property_id) { return property_has_quirk(property_id, Quirk::HashlessHexColor); },
                [](FunctionContext const&) { return false; });
        }
        for (auto i = 1u; i < m_value_context.size() && quirky_color_allowed; i++) {
            quirky_color_allowed = m_value_context[i].visit(
                [](PropertyID const& property_id) { return property_has_quirk(property_id, Quirk::UnitlessLength); },
                [](FunctionContext const&) {
                    return false;
                });
        }
        if (quirky_color_allowed) {
            // NOTE: This algorithm is no longer in the spec, since the concept got moved and renamed. However, it works,
            //       and so we might as well keep using it.

            // The value of a quirky color is obtained from the possible component values using the following algorithm,
            // aborting on the first step that returns a value:

            // 1. Let cv be the component value.
            auto const& cv = component_value;
            String serialization;
            // 2. If cv is a <number-token> or a <dimension-token>, follow these substeps:
            if (cv.is(Token::Type::Number) || cv.is(Token::Type::Dimension)) {
                // 1. If cv’s type flag is not "integer", return an error.
                //    This means that values that happen to use scientific notation, e.g., 5e5e5e, will fail to parse.
                if (!cv.token().number().is_integer())
                    return {};

                // 2. If cv’s value is less than zero, return an error.
                auto value = cv.is(Token::Type::Number) ? cv.token().to_integer() : cv.token().dimension_value_int();
                if (value < 0)
                    return {};

                // 3. Let serialization be the serialization of cv’s value, as a base-ten integer using digits 0-9 (U+0030 to U+0039) in the shortest form possible.
                StringBuilder serialization_builder;
                serialization_builder.appendff("{}", value);

                // 4. If cv is a <dimension-token>, append the unit to serialization.
                if (cv.is(Token::Type::Dimension))
                    serialization_builder.append(cv.token().dimension_unit());

                // 5. If serialization consists of fewer than six characters, prepend zeros (U+0030) so that it becomes six characters.
                serialization = MUST(serialization_builder.to_string());
                if (serialization_builder.length() < 6) {
                    StringBuilder builder;
                    for (size_t i = 0; i < (6 - serialization_builder.length()); i++)
                        builder.append('0');
                    builder.append(serialization_builder.string_view());
                    serialization = MUST(builder.to_string());
                }
            }
            // 3. Otherwise, cv is an <ident-token>; let serialization be cv’s value.
            else {
                if (!cv.is(Token::Type::Ident))
                    return {};
                serialization = cv.token().ident().to_string();
            }

            // 4. If serialization does not consist of three or six characters, return an error.
            if (serialization.bytes().size() != 3 && serialization.bytes().size() != 6)
                return {};

            // 5. If serialization contains any characters not in the range [0-9A-Fa-f] (U+0030 to U+0039, U+0041 to U+0046, U+0061 to U+0066), return an error.
            for (auto c : serialization.bytes_as_string_view()) {
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
                    return {};
            }

            // 6. Return the concatenation of "#" (U+0023) and serialization.
            auto color = Color::from_string(MUST(String::formatted("#{}", serialization)));
            if (color.has_value()) {
                transaction.commit();
                return CSSColorValue::create_from_color(color.release_value(), ColorSyntax::Legacy);
            }
        }
    }

    return {};
}

// https://drafts.csswg.org/css-lists-3/#counter-functions
RefPtr<CSSStyleValue> Parser::parse_counter_value(TokenStream<ComponentValue>& tokens)
{
    auto parse_counter_name = [this](TokenStream<ComponentValue>& tokens) -> Optional<FlyString> {
        // https://drafts.csswg.org/css-lists-3/#typedef-counter-name
        // Counters are referred to in CSS syntax using the <counter-name> type, which represents
        // their name as a <custom-ident>. A <counter-name> name cannot match the keyword none;
        // such an identifier is invalid as a <counter-name>.
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        auto counter_name = parse_custom_ident_value(tokens, { { "none"sv } });
        if (!counter_name)
            return {};

        tokens.discard_whitespace();
        if (tokens.has_next_token())
            return {};

        transaction.commit();
        return counter_name->custom_ident();
    };

    auto parse_counter_style = [this](TokenStream<ComponentValue>& tokens) -> RefPtr<CSSStyleValue> {
        // https://drafts.csswg.org/css-counter-styles-3/#typedef-counter-style
        // <counter-style> = <counter-style-name> | <symbols()>
        // For now we just support <counter-style-name>, found here:
        // https://drafts.csswg.org/css-counter-styles-3/#typedef-counter-style-name
        // <counter-style-name> is a <custom-ident> that is not an ASCII case-insensitive match for none.
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        auto counter_style_name = parse_custom_ident_value(tokens, { { "none"sv } });
        if (!counter_style_name)
            return {};

        tokens.discard_whitespace();
        if (tokens.has_next_token())
            return {};

        transaction.commit();
        return counter_style_name.release_nonnull();
    };

    auto transaction = tokens.begin_transaction();
    auto const& token = tokens.consume_a_token();
    if (token.is_function("counter"sv)) {
        // counter() = counter( <counter-name>, <counter-style>? )
        auto& function = token.function();
        auto context_guard = push_temporary_value_parsing_context(FunctionContext { function.name });

        TokenStream function_tokens { function.value };
        auto function_values = parse_a_comma_separated_list_of_component_values(function_tokens);
        if (function_values.is_empty() || function_values.size() > 2)
            return nullptr;

        TokenStream name_tokens { function_values[0] };
        auto counter_name = parse_counter_name(name_tokens);
        if (!counter_name.has_value())
            return nullptr;

        RefPtr<CSSStyleValue> counter_style;
        if (function_values.size() > 1) {
            TokenStream counter_style_tokens { function_values[1] };
            counter_style = parse_counter_style(counter_style_tokens);
            if (!counter_style)
                return nullptr;
        } else {
            // In both cases, if the <counter-style> argument is omitted it defaults to `decimal`.
            counter_style = CustomIdentStyleValue::create("decimal"_fly_string);
        }

        transaction.commit();
        return CounterStyleValue::create_counter(counter_name.release_value(), counter_style.release_nonnull());
    }

    if (token.is_function("counters"sv)) {
        // counters() = counters( <counter-name>, <string>, <counter-style>? )
        auto& function = token.function();
        auto context_guard = push_temporary_value_parsing_context(FunctionContext { function.name });

        TokenStream function_tokens { function.value };
        auto function_values = parse_a_comma_separated_list_of_component_values(function_tokens);
        if (function_values.size() < 2 || function_values.size() > 3)
            return nullptr;

        TokenStream name_tokens { function_values[0] };
        auto counter_name = parse_counter_name(name_tokens);
        if (!counter_name.has_value())
            return nullptr;

        TokenStream string_tokens { function_values[1] };
        string_tokens.discard_whitespace();
        auto join_string = parse_string_value(string_tokens);
        string_tokens.discard_whitespace();
        if (!join_string || string_tokens.has_next_token())
            return nullptr;

        RefPtr<CSSStyleValue> counter_style;
        if (function_values.size() > 2) {
            TokenStream counter_style_tokens { function_values[2] };
            counter_style = parse_counter_style(counter_style_tokens);
            if (!counter_style)
                return nullptr;
        } else {
            // In both cases, if the <counter-style> argument is omitted it defaults to `decimal`.
            counter_style = CustomIdentStyleValue::create("decimal"_fly_string);
        }

        transaction.commit();
        return CounterStyleValue::create_counters(counter_name.release_value(), join_string->string_value(), counter_style.release_nonnull());
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_ratio_value(TokenStream<ComponentValue>& tokens)
{
    if (auto ratio = parse_ratio(tokens); ratio.has_value())
        return RatioStyleValue::create(ratio.release_value());
    return nullptr;
}

RefPtr<StringStyleValue> Parser::parse_string_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek = tokens.next_token();
    if (peek.is(Token::Type::String)) {
        tokens.discard_a_token();
        return StringStyleValue::create(peek.token().string());
    }

    return nullptr;
}

RefPtr<AbstractImageStyleValue> Parser::parse_image_value(TokenStream<ComponentValue>& tokens)
{
    tokens.mark();
    auto url = parse_url_function(tokens);
    if (url.has_value()) {
        // If the value is a 'url(..)' parse as image, but if it is just a reference 'url(#xx)', leave it alone,
        // so we can parse as URL further on. These URLs are used as references inside SVG documents for masks.
        if (!url.value().equals(m_url, URL::ExcludeFragment::Yes)) {
            tokens.discard_a_mark();
            return ImageStyleValue::create(url.value());
        }
        tokens.restore_a_mark();
        return nullptr;
    }
    tokens.discard_a_mark();

    if (auto linear_gradient = parse_linear_gradient_function(tokens))
        return linear_gradient;

    if (auto conic_gradient = parse_conic_gradient_function(tokens))
        return conic_gradient;

    if (auto radial_gradient = parse_radial_gradient_function(tokens))
        return radial_gradient;

    return nullptr;
}

// https://svgwg.org/svg2-draft/painting.html#SpecifyingPaint
RefPtr<CSSStyleValue> Parser::parse_paint_value(TokenStream<ComponentValue>& tokens)
{
    // `<paint> = none | <color> | <url> [none | <color>]? | context-fill | context-stroke`

    auto parse_color_or_none = [&]() -> Optional<RefPtr<CSSStyleValue>> {
        if (auto color = parse_color_value(tokens))
            return color;

        // NOTE: <color> also accepts identifiers, so we do this identifier check last.
        if (tokens.next_token().is(Token::Type::Ident)) {
            auto maybe_keyword = keyword_from_string(tokens.next_token().token().ident());
            if (maybe_keyword.has_value()) {
                // FIXME: Accept `context-fill` and `context-stroke`
                switch (*maybe_keyword) {
                case Keyword::None:
                    tokens.discard_a_token();
                    return CSSKeywordValue::create(*maybe_keyword);
                default:
                    return nullptr;
                }
            }
        }

        return OptionalNone {};
    };

    // FIMXE: Allow context-fill/context-stroke here
    if (auto color_or_none = parse_color_or_none(); color_or_none.has_value())
        return *color_or_none;

    if (auto url = parse_url_value(tokens)) {
        tokens.discard_whitespace();
        if (auto color_or_none = parse_color_or_none(); color_or_none == nullptr) {
            // Fail to parse if the fallback is invalid, but otherwise ignore it.
            // FIXME: Use fallback color
            return nullptr;
        }
        return url;
    }

    return nullptr;
}

// https://www.w3.org/TR/css-values-4/#position
RefPtr<PositionStyleValue> Parser::parse_position_value(TokenStream<ComponentValue>& tokens, PositionParsingMode position_parsing_mode)
{
    auto parse_position_edge = [](TokenStream<ComponentValue>& tokens) -> Optional<PositionEdge> {
        auto transaction = tokens.begin_transaction();
        auto& token = tokens.consume_a_token();
        if (!token.is(Token::Type::Ident))
            return {};
        auto keyword = keyword_from_string(token.token().ident());
        if (!keyword.has_value())
            return {};
        transaction.commit();
        return keyword_to_position_edge(*keyword);
    };

    auto is_horizontal = [](PositionEdge edge, bool accept_center) -> bool {
        switch (edge) {
        case PositionEdge::Left:
        case PositionEdge::Right:
            return true;
        case PositionEdge::Center:
            return accept_center;
        default:
            return false;
        }
    };

    auto is_vertical = [](PositionEdge edge, bool accept_center) -> bool {
        switch (edge) {
        case PositionEdge::Top:
        case PositionEdge::Bottom:
            return true;
        case PositionEdge::Center:
            return accept_center;
        default:
            return false;
        }
    };

    // <position> = [
    //   [ left | center | right | top | bottom | <length-percentage> ]
    // |
    //   [ left | center | right ] && [ top | center | bottom ]
    // |
    //   [ left | center | right | <length-percentage> ]
    //   [ top | center | bottom | <length-percentage> ]
    // |
    //   [ [ left | right ] <length-percentage> ] &&
    //   [ [ top | bottom ] <length-percentage> ]
    // ]

    // [ left | center | right | top | bottom | <length-percentage> ]
    auto alternative_1 = [&]() -> RefPtr<PositionStyleValue> {
        auto transaction = tokens.begin_transaction();

        tokens.discard_whitespace();

        // [ left | center | right | top | bottom ]
        if (auto maybe_edge = parse_position_edge(tokens); maybe_edge.has_value()) {
            auto edge = maybe_edge.release_value();
            transaction.commit();

            // [ left | right ]
            if (is_horizontal(edge, false))
                return PositionStyleValue::create(EdgeStyleValue::create(edge, {}), EdgeStyleValue::create(PositionEdge::Center, {}));

            // [ top | bottom ]
            if (is_vertical(edge, false))
                return PositionStyleValue::create(EdgeStyleValue::create(PositionEdge::Center, {}), EdgeStyleValue::create(edge, {}));

            // [ center ]
            VERIFY(edge == PositionEdge::Center);
            return PositionStyleValue::create(EdgeStyleValue::create(PositionEdge::Center, {}), EdgeStyleValue::create(PositionEdge::Center, {}));
        }

        // [ <length-percentage> ]
        if (auto maybe_percentage = parse_length_percentage(tokens); maybe_percentage.has_value()) {
            transaction.commit();
            return PositionStyleValue::create(EdgeStyleValue::create({}, *maybe_percentage), EdgeStyleValue::create(PositionEdge::Center, {}));
        }

        return nullptr;
    };

    // [ left | center | right ] && [ top | center | bottom ]
    auto alternative_2 = [&]() -> RefPtr<PositionStyleValue> {
        auto transaction = tokens.begin_transaction();

        tokens.discard_whitespace();

        // Parse out two position edges
        auto maybe_first_edge = parse_position_edge(tokens);
        if (!maybe_first_edge.has_value())
            return nullptr;

        auto first_edge = maybe_first_edge.release_value();
        tokens.discard_whitespace();

        auto maybe_second_edge = parse_position_edge(tokens);
        if (!maybe_second_edge.has_value())
            return nullptr;

        auto second_edge = maybe_second_edge.release_value();

        // If 'left' or 'right' is given, that position is X and the other is Y.
        // Conversely -
        // If 'top' or 'bottom' is given, that position is Y and the other is X.
        if (is_vertical(first_edge, false) || is_horizontal(second_edge, false))
            swap(first_edge, second_edge);

        // [ left | center | right ] [ top | bottom | center ]
        if (is_horizontal(first_edge, true) && is_vertical(second_edge, true)) {
            transaction.commit();
            return PositionStyleValue::create(EdgeStyleValue::create(first_edge, {}), EdgeStyleValue::create(second_edge, {}));
        }

        return nullptr;
    };

    // [ left | center | right | <length-percentage> ]
    // [ top | center | bottom | <length-percentage> ]
    auto alternative_3 = [&]() -> RefPtr<PositionStyleValue> {
        auto transaction = tokens.begin_transaction();

        auto parse_position_or_length = [&](bool as_horizontal) -> RefPtr<EdgeStyleValue> {
            tokens.discard_whitespace();

            if (auto maybe_position = parse_position_edge(tokens); maybe_position.has_value()) {
                auto position = maybe_position.release_value();
                bool valid = as_horizontal ? is_horizontal(position, true) : is_vertical(position, true);
                if (!valid)
                    return nullptr;
                return EdgeStyleValue::create(position, {});
            }

            auto maybe_length = parse_length_percentage(tokens);
            if (!maybe_length.has_value())
                return nullptr;

            return EdgeStyleValue::create({}, maybe_length);
        };

        // [ left | center | right | <length-percentage> ]
        auto horizontal_edge = parse_position_or_length(true);
        if (!horizontal_edge)
            return nullptr;

        // [ top | center | bottom | <length-percentage> ]
        auto vertical_edge = parse_position_or_length(false);
        if (!vertical_edge)
            return nullptr;

        transaction.commit();
        return PositionStyleValue::create(horizontal_edge.release_nonnull(), vertical_edge.release_nonnull());
    };

    // [ [ left | right ] <length-percentage> ] &&
    // [ [ top | bottom ] <length-percentage> ]
    auto alternative_4 = [&]() -> RefPtr<PositionStyleValue> {
        struct PositionAndLength {
            PositionEdge position;
            LengthPercentage length;
        };

        auto parse_position_and_length = [&]() -> Optional<PositionAndLength> {
            tokens.discard_whitespace();

            auto maybe_position = parse_position_edge(tokens);
            if (!maybe_position.has_value())
                return {};

            tokens.discard_whitespace();

            auto maybe_length = parse_length_percentage(tokens);
            if (!maybe_length.has_value())
                return {};

            return PositionAndLength {
                .position = maybe_position.release_value(),
                .length = maybe_length.release_value(),
            };
        };

        auto transaction = tokens.begin_transaction();

        auto maybe_group1 = parse_position_and_length();
        if (!maybe_group1.has_value())
            return nullptr;

        auto maybe_group2 = parse_position_and_length();
        if (!maybe_group2.has_value())
            return nullptr;

        auto group1 = maybe_group1.release_value();
        auto group2 = maybe_group2.release_value();

        // [ [ left | right ] <length-percentage> ] [ [ top | bottom ] <length-percentage> ]
        if (is_horizontal(group1.position, false) && is_vertical(group2.position, false)) {
            transaction.commit();
            return PositionStyleValue::create(EdgeStyleValue::create(group1.position, group1.length), EdgeStyleValue::create(group2.position, group2.length));
        }

        // [ [ top | bottom ] <length-percentage> ] [ [ left | right ] <length-percentage> ]
        if (is_vertical(group1.position, false) && is_horizontal(group2.position, false)) {
            transaction.commit();
            return PositionStyleValue::create(EdgeStyleValue::create(group2.position, group2.length), EdgeStyleValue::create(group1.position, group1.length));
        }

        return nullptr;
    };

    // The extra 3-value syntax that's allowed for background-position:
    // [ center | [ left | right ] <length-percentage>? ] &&
    // [ center | [ top | bottom ] <length-percentage>? ]
    auto alternative_5_for_background_position = [&]() -> RefPtr<PositionStyleValue> {
        auto transaction = tokens.begin_transaction();

        struct PositionAndMaybeLength {
            PositionEdge position;
            Optional<LengthPercentage> length;
        };

        // [ <position> <length-percentage>? ]
        auto parse_position_and_maybe_length = [&]() -> Optional<PositionAndMaybeLength> {
            auto inner_transaction = tokens.begin_transaction();
            tokens.discard_whitespace();

            auto maybe_position = parse_position_edge(tokens);
            if (!maybe_position.has_value())
                return {};

            tokens.discard_whitespace();

            auto maybe_length = parse_length_percentage(tokens);
            if (maybe_length.has_value()) {
                // 'center' cannot be followed by a <length-percentage>
                if (maybe_position.value() == PositionEdge::Center && maybe_length.has_value())
                    return {};
            }

            inner_transaction.commit();
            return PositionAndMaybeLength {
                .position = maybe_position.release_value(),
                .length = move(maybe_length),
            };
        };

        auto maybe_group1 = parse_position_and_maybe_length();
        if (!maybe_group1.has_value())
            return nullptr;

        auto maybe_group2 = parse_position_and_maybe_length();
        if (!maybe_group2.has_value())
            return nullptr;

        auto group1 = maybe_group1.release_value();
        auto group2 = maybe_group2.release_value();

        // 2-value or 4-value if both <length-percentage>s are present or missing.
        if (group1.length.has_value() == group2.length.has_value())
            return nullptr;

        // If 'left' or 'right' is given, that position is X and the other is Y.
        // Conversely -
        // If 'top' or 'bottom' is given, that position is Y and the other is X.
        if (is_vertical(group1.position, false) || is_horizontal(group2.position, false))
            swap(group1, group2);

        // [ center | [ left | right ] ]
        if (!is_horizontal(group1.position, true))
            return nullptr;

        // [ center | [ top | bottom ] ]
        if (!is_vertical(group2.position, true))
            return nullptr;

        auto to_style_value = [&](PositionAndMaybeLength const& group) -> NonnullRefPtr<EdgeStyleValue> {
            if (group.position == PositionEdge::Center)
                return EdgeStyleValue::create(PositionEdge::Center, {});

            return EdgeStyleValue::create(group.position, group.length);
        };

        transaction.commit();
        return PositionStyleValue::create(to_style_value(group1), to_style_value(group2));
    };

    // Note: The alternatives must be attempted in this order since shorter alternatives can match a prefix of longer ones.
    if (auto position = alternative_4())
        return position;
    if (position_parsing_mode == PositionParsingMode::BackgroundPosition) {
        if (auto position = alternative_5_for_background_position())
            return position;
    }
    if (auto position = alternative_3())
        return position;
    if (auto position = alternative_2())
        return position;
    if (auto position = alternative_1())
        return position;
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_easing_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    tokens.discard_whitespace();

    auto const& part = tokens.consume_a_token();

    if (part.is(Token::Type::Ident)) {
        auto name = part.token().ident();
        auto maybe_simple_easing = [&] -> RefPtr<EasingStyleValue> {
            if (name.equals_ignoring_ascii_case("linear"sv))
                return EasingStyleValue::create(EasingStyleValue::Linear::identity());
            if (name.equals_ignoring_ascii_case("ease"sv))
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease());
            if (name.equals_ignoring_ascii_case("ease-in"sv))
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease_in());
            if (name.equals_ignoring_ascii_case("ease-out"sv))
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease_out());
            if (name.equals_ignoring_ascii_case("ease-in-out"sv))
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease_in_out());
            if (name.equals_ignoring_ascii_case("step-start"sv))
                return EasingStyleValue::create(EasingStyleValue::Steps::step_start());
            if (name.equals_ignoring_ascii_case("step-end"sv))
                return EasingStyleValue::create(EasingStyleValue::Steps::step_end());
            return {};
        }();

        if (!maybe_simple_easing)
            return nullptr;

        transaction.commit();
        return maybe_simple_easing;
    }

    if (!part.is_function())
        return nullptr;

    TokenStream argument_tokens { part.function().value };
    auto comma_separated_arguments = parse_a_comma_separated_list_of_component_values(argument_tokens);

    // Remove whitespace
    for (auto& argument : comma_separated_arguments)
        argument.remove_all_matching([](auto& value) { return value.is(Token::Type::Whitespace); });

    auto name = part.function().name;
    auto context_guard = push_temporary_value_parsing_context(FunctionContext { name });

    if (name.equals_ignoring_ascii_case("linear"sv)) {
        // linear() = linear( [ <number> && <percentage>{0,2} ]# )
        Vector<EasingStyleValue::Linear::Stop> stops;
        for (auto const& argument : comma_separated_arguments) {
            TokenStream argument_tokens { argument };

            Optional<double> output;
            Optional<double> first_input;
            Optional<double> second_input;

            if (argument_tokens.next_token().is(Token::Type::Number))
                output = argument_tokens.consume_a_token().token().number_value();

            if (argument_tokens.next_token().is(Token::Type::Percentage)) {
                first_input = argument_tokens.consume_a_token().token().percentage() / 100;
                if (argument_tokens.next_token().is(Token::Type::Percentage)) {
                    second_input = argument_tokens.consume_a_token().token().percentage() / 100;
                }
            }

            if (argument_tokens.next_token().is(Token::Type::Number)) {
                if (output.has_value())
                    return nullptr;
                output = argument_tokens.consume_a_token().token().number_value();
            }

            if (argument_tokens.has_next_token() || !output.has_value())
                return nullptr;

            stops.append({ output.value(), first_input, first_input.has_value() });
            if (second_input.has_value())
                stops.append({ output.value(), second_input, true });
        }

        if (stops.is_empty())
            return nullptr;

        transaction.commit();
        return EasingStyleValue::create(EasingStyleValue::Linear { move(stops) });
    }

    if (name.equals_ignoring_ascii_case("cubic-bezier"sv)) {
        if (comma_separated_arguments.size() != 4)
            return nullptr;

        for (auto const& argument : comma_separated_arguments) {
            if (argument.size() != 1)
                return nullptr;
            if (!argument[0].is(Token::Type::Number))
                return nullptr;
        }

        EasingStyleValue::CubicBezier bezier {
            comma_separated_arguments[0][0].token().number_value(),
            comma_separated_arguments[1][0].token().number_value(),
            comma_separated_arguments[2][0].token().number_value(),
            comma_separated_arguments[3][0].token().number_value(),
        };

        if (bezier.x1 < 0.0 || bezier.x1 > 1.0 || bezier.x2 < 0.0 || bezier.x2 > 1.0)
            return nullptr;

        transaction.commit();
        return EasingStyleValue::create(bezier);
    }

    if (name.equals_ignoring_ascii_case("steps"sv)) {
        if (comma_separated_arguments.is_empty() || comma_separated_arguments.size() > 2)
            return nullptr;

        for (auto const& argument : comma_separated_arguments) {
            if (argument.size() != 1)
                return nullptr;
        }

        EasingStyleValue::Steps steps;

        auto const& intervals_argument = comma_separated_arguments[0][0];
        if (!intervals_argument.is(Token::Type::Number))
            return nullptr;
        if (!intervals_argument.token().number().is_integer())
            return nullptr;
        auto intervals = intervals_argument.token().to_integer();

        if (comma_separated_arguments.size() == 2) {
            TokenStream identifier_stream { comma_separated_arguments[1] };
            auto keyword_value = parse_keyword_value(identifier_stream);
            if (!keyword_value)
                return nullptr;
            switch (keyword_value->to_keyword()) {
            case Keyword::JumpStart:
                steps.position = EasingStyleValue::Steps::Position::JumpStart;
                break;
            case Keyword::JumpEnd:
                steps.position = EasingStyleValue::Steps::Position::JumpEnd;
                break;
            case Keyword::JumpBoth:
                steps.position = EasingStyleValue::Steps::Position::JumpBoth;
                break;
            case Keyword::JumpNone:
                steps.position = EasingStyleValue::Steps::Position::JumpNone;
                break;
            case Keyword::Start:
                steps.position = EasingStyleValue::Steps::Position::Start;
                break;
            case Keyword::End:
                steps.position = EasingStyleValue::Steps::Position::End;
                break;
            default:
                return nullptr;
            }
        }

        // Perform extra validation
        // https://drafts.csswg.org/css-easing/#step-easing-functions
        // If the <step-position> is jump-none, the <integer> must be at least 2, or the function is invalid.
        // Otherwise, the <integer> must be at least 1, or the function is invalid.
        if (steps.position == EasingStyleValue::Steps::Position::JumpNone) {
            if (intervals <= 1)
                return nullptr;
        } else if (intervals <= 0) {
            return nullptr;
        }

        steps.number_of_intervals = intervals;
        transaction.commit();
        return EasingStyleValue::create(steps);
    }

    return nullptr;
}

Optional<URL::URL> Parser::parse_url_function(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();

    auto convert_string_to_url = [&](StringView url_string) -> Optional<URL::URL> {
        auto url = complete_url(url_string);
        if (url.has_value()) {
            transaction.commit();
            return url;
        }
        return {};
    };

    if (component_value.is(Token::Type::Url)) {
        auto url_string = component_value.token().url();
        return convert_string_to_url(url_string);
    }
    if (component_value.is_function("url"sv)) {
        auto const& function_values = component_value.function().value;
        // FIXME: Handle url-modifiers. https://www.w3.org/TR/css-values-4/#url-modifiers
        for (size_t i = 0; i < function_values.size(); ++i) {
            auto const& value = function_values[i];
            if (value.is(Token::Type::Whitespace))
                continue;
            if (value.is(Token::Type::String)) {
                auto url_string = value.token().string();
                return convert_string_to_url(url_string);
            }
            break;
        }
    }

    return {};
}

RefPtr<CSSStyleValue> Parser::parse_url_value(TokenStream<ComponentValue>& tokens)
{
    auto url = parse_url_function(tokens);
    if (!url.has_value())
        return nullptr;
    return URLStyleValue::create(*url);
}

// https://www.w3.org/TR/css-shapes-1/#typedef-shape-radius
Optional<ShapeRadius> Parser::parse_shape_radius(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto maybe_radius = parse_length_percentage(tokens);
    if (maybe_radius.has_value()) {
        // Negative radius is invalid.
        auto radius = maybe_radius.value();
        if ((radius.is_length() && radius.length().raw_value() < 0) || (radius.is_percentage() && radius.percentage().value() < 0))
            return {};

        transaction.commit();
        return radius;
    }

    if (tokens.next_token().is_ident("closest-side"sv)) {
        tokens.discard_a_token();
        transaction.commit();
        return FitSide::ClosestSide;
    }

    if (tokens.next_token().is_ident("farthest-side"sv)) {
        tokens.discard_a_token();
        transaction.commit();
        return FitSide::FarthestSide;
    }

    return {};
}

RefPtr<FitContentStyleValue> Parser::parse_fit_content_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();

    if (component_value.is_ident("fit-content"sv)) {
        transaction.commit();
        return FitContentStyleValue::create();
        return nullptr;
    }

    if (!component_value.is_function())
        return nullptr;

    auto const& function = component_value.function();
    if (function.name != "fit-content"sv)
        return nullptr;
    TokenStream argument_tokens { function.value };
    argument_tokens.discard_whitespace();
    auto maybe_length = parse_length_percentage(argument_tokens);
    if (!maybe_length.has_value())
        return nullptr;
    argument_tokens.discard_whitespace();
    if (argument_tokens.has_next_token())
        return nullptr;

    transaction.commit();
    return FitContentStyleValue::create(maybe_length.release_value());
}

RefPtr<CSSStyleValue> Parser::parse_basic_shape_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();
    if (!component_value.is_function())
        return nullptr;

    auto function_name = component_value.function().name.bytes_as_string_view();

    // FIXME: Implement path(). See: https://www.w3.org/TR/css-shapes-1/#basic-shape-functions
    if (function_name.equals_ignoring_ascii_case("inset"sv)) {
        // inset() = inset( <length-percentage>{1,4} [ round <'border-radius'> ]? )
        // FIXME: Parse the border-radius.
        auto arguments_tokens = TokenStream { component_value.function().value };

        // If less than four <length-percentage> values are provided,
        // the omitted values default in the same way as the margin shorthand:
        // an omitted second or third value defaults to the first, and an omitted fourth value defaults to the second.

        // The four <length-percentage>s define the position of the top, right, bottom, and left edges of a rectangle.

        arguments_tokens.discard_whitespace();
        auto top = parse_length_percentage(arguments_tokens);
        if (!top.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        auto right = parse_length_percentage(arguments_tokens);
        if (!right.has_value())
            right = top;

        arguments_tokens.discard_whitespace();
        auto bottom = parse_length_percentage(arguments_tokens);
        if (!bottom.has_value())
            bottom = top;

        arguments_tokens.discard_whitespace();
        auto left = parse_length_percentage(arguments_tokens);
        if (!left.has_value())
            left = right;

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Inset { LengthBox(top.value(), right.value(), bottom.value(), left.value()) });
    }

    if (function_name.equals_ignoring_ascii_case("xywh"sv)) {
        // xywh() = xywh( <length-percentage>{2} <length-percentage [0,∞]>{2} [ round <'border-radius'> ]? )
        // FIXME: Parse the border-radius.
        auto arguments_tokens = TokenStream { component_value.function().value };

        arguments_tokens.discard_whitespace();
        auto x = parse_length_percentage(arguments_tokens);
        if (!x.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        auto y = parse_length_percentage(arguments_tokens);
        if (!y.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        auto width = parse_length_percentage(arguments_tokens);
        if (!width.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        auto height = parse_length_percentage(arguments_tokens);
        if (!height.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        // Negative width or height is invalid.
        if ((width->is_length() && width->length().raw_value() < 0)
            || (width->is_percentage() && width->percentage().value() < 0)
            || (height->is_length() && height->length().raw_value() < 0)
            || (height->is_percentage() && height->percentage().value() < 0))
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Xywh { x.value(), y.value(), width.value(), height.value() });
    }

    if (function_name.equals_ignoring_ascii_case("rect"sv)) {
        // rect() = rect( [ <length-percentage> | auto ]{4} [ round <'border-radius'> ]? )
        // FIXME: Parse the border-radius.
        auto arguments_tokens = TokenStream { component_value.function().value };

        auto parse_length_percentage_or_auto = [this](TokenStream<ComponentValue>& tokens) -> Optional<LengthPercentage> {
            tokens.discard_whitespace();
            auto value = parse_length_percentage(tokens);
            if (!value.has_value()) {
                if (tokens.consume_a_token().is_ident("auto"sv)) {
                    value = Length::make_auto();
                }
            }
            return value;
        };

        auto top = parse_length_percentage_or_auto(arguments_tokens);
        auto right = parse_length_percentage_or_auto(arguments_tokens);
        auto bottom = parse_length_percentage_or_auto(arguments_tokens);
        auto left = parse_length_percentage_or_auto(arguments_tokens);

        if (!top.has_value() || !right.has_value() || !bottom.has_value() || !left.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Rect { LengthBox(top.value(), right.value(), bottom.value(), left.value()) });
    }

    if (function_name.equals_ignoring_ascii_case("circle"sv)) {
        // circle() = circle( <shape-radius>? [ at <position> ]? )
        auto arguments_tokens = TokenStream { component_value.function().value };

        auto radius = parse_shape_radius(arguments_tokens).value_or(FitSide::ClosestSide);

        auto position = PositionStyleValue::create_center();
        arguments_tokens.discard_whitespace();
        if (arguments_tokens.next_token().is_ident("at"sv)) {
            arguments_tokens.discard_a_token();
            arguments_tokens.discard_whitespace();
            auto maybe_position = parse_position_value(arguments_tokens);
            if (maybe_position.is_null())
                return nullptr;

            position = maybe_position.release_nonnull();
        }

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Circle { radius, position });
    }

    if (function_name.equals_ignoring_ascii_case("ellipse"sv)) {
        // ellipse() = ellipse( [ <shape-radius>{2} ]? [ at <position> ]? )
        auto arguments_tokens = TokenStream { component_value.function().value };

        Optional<ShapeRadius> radius_x = parse_shape_radius(arguments_tokens);
        Optional<ShapeRadius> radius_y = parse_shape_radius(arguments_tokens);

        if (radius_x.has_value() && !radius_y.has_value())
            return nullptr;

        if (!radius_x.has_value()) {
            radius_x = FitSide::ClosestSide;
            radius_y = FitSide::ClosestSide;
        }

        auto position = PositionStyleValue::create_center();
        arguments_tokens.discard_whitespace();
        if (arguments_tokens.next_token().is_ident("at"sv)) {
            arguments_tokens.discard_a_token();
            arguments_tokens.discard_whitespace();
            auto maybe_position = parse_position_value(arguments_tokens);
            if (maybe_position.is_null())
                return nullptr;

            position = maybe_position.release_nonnull();
        }

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Ellipse { radius_x.value(), radius_y.value(), position });
    }

    if (function_name.equals_ignoring_ascii_case("polygon"sv)) {
        // polygon() = polygon( <'fill-rule'>? , [<length-percentage> <length-percentage>]# )
        auto arguments_tokens = TokenStream { component_value.function().value };
        auto arguments = parse_a_comma_separated_list_of_component_values(arguments_tokens);

        if (arguments.size() < 1)
            return nullptr;

        Optional<Gfx::WindingRule> fill_rule;
        auto const& first_argument = arguments[0];
        TokenStream first_argument_tokens { first_argument };

        first_argument_tokens.discard_whitespace();
        if (first_argument_tokens.next_token().is_ident("nonzero"sv)) {
            fill_rule = Gfx::WindingRule::Nonzero;
        } else if (first_argument_tokens.next_token().is_ident("evenodd"sv)) {
            fill_rule = Gfx::WindingRule::EvenOdd;
        }

        if (fill_rule.has_value()) {
            first_argument_tokens.discard_a_token();
            if (first_argument_tokens.has_next_token())
                return nullptr;

            arguments.remove(0);
        } else {
            fill_rule = Gfx::WindingRule::Nonzero;
        }

        if (arguments.size() < 1)
            return nullptr;

        Vector<Polygon::Point> points;
        for (auto& argument : arguments) {
            TokenStream argument_tokens { argument };

            argument_tokens.discard_whitespace();
            auto x_pos = parse_length_percentage(argument_tokens);
            if (!x_pos.has_value())
                return nullptr;

            argument_tokens.discard_whitespace();
            auto y_pos = parse_length_percentage(argument_tokens);
            if (!y_pos.has_value())
                return nullptr;

            argument_tokens.discard_whitespace();
            if (argument_tokens.has_next_token())
                return nullptr;

            points.append(Polygon::Point { *x_pos, *y_pos });
        }

        transaction.commit();
        return BasicShapeStyleValue::create(Polygon { fill_rule.value(), move(points) });
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_builtin_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();
    if (component_value.is(Token::Type::Ident)) {
        auto ident = component_value.token().ident();
        if (ident.equals_ignoring_ascii_case("inherit"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::Inherit);
        }
        if (ident.equals_ignoring_ascii_case("initial"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::Initial);
        }
        if (ident.equals_ignoring_ascii_case("unset"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::Unset);
        }
        if (ident.equals_ignoring_ascii_case("revert"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::Revert);
        }
        if (ident.equals_ignoring_ascii_case("revert-layer"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::RevertLayer);
        }
    }

    return nullptr;
}

// https://www.w3.org/TR/css-values-4/#custom-idents
Optional<FlyString> Parser::parse_custom_ident(TokenStream<ComponentValue>& tokens, ReadonlySpan<StringView> blacklist)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    auto const& token = tokens.consume_a_token();
    if (!token.is(Token::Type::Ident))
        return {};
    auto custom_ident = token.token().ident();

    // The CSS-wide keywords are not valid <custom-ident>s.
    if (is_css_wide_keyword(custom_ident))
        return {};

    // The default keyword is reserved and is also not a valid <custom-ident>.
    if (custom_ident.equals_ignoring_ascii_case("default"sv))
        return {};

    // Specifications using <custom-ident> must specify clearly what other keywords are excluded from <custom-ident>,
    // if any—for example by saying that any pre-defined keywords in that property’s value definition are excluded.
    // Excluded keywords are excluded in all ASCII case permutations.
    for (auto& value : blacklist) {
        if (custom_ident.equals_ignoring_ascii_case(value))
            return {};
    }

    transaction.commit();
    return custom_ident;
}

RefPtr<CustomIdentStyleValue> Parser::parse_custom_ident_value(TokenStream<ComponentValue>& tokens, ReadonlySpan<StringView> blacklist)
{
    if (auto custom_ident = parse_custom_ident(tokens, blacklist); custom_ident.has_value())
        return CustomIdentStyleValue::create(custom_ident.release_value());
    return nullptr;
}

Optional<CSS::GridSize> Parser::parse_grid_size(ComponentValue const& component_value)
{
    if (component_value.is_function()) {
        if (auto maybe_calculated = parse_calculated_value(component_value)) {
            if (maybe_calculated->is_length())
                return GridSize(maybe_calculated->as_length().length());
            if (maybe_calculated->is_percentage())
                return GridSize(maybe_calculated->as_percentage().percentage());
            if (maybe_calculated->is_calculated() && maybe_calculated->as_calculated().resolves_to_length_percentage())
                return GridSize(LengthPercentage(maybe_calculated->as_calculated()));
            // FIXME: Support calculated <flex>
        }

        return {};
    }
    if (component_value.is_ident("auto"sv))
        return GridSize::make_auto();
    if (component_value.is_ident("max-content"sv))
        return GridSize(GridSize::Type::MaxContent);
    if (component_value.is_ident("min-content"sv))
        return GridSize(GridSize::Type::MinContent);
    auto dimension = parse_dimension(component_value);
    if (!dimension.has_value())
        return {};
    if (dimension->is_length())
        return GridSize(dimension->length());
    else if (dimension->is_percentage())
        return GridSize(dimension->percentage());
    else if (dimension->is_flex())
        return GridSize(dimension->flex());
    return {};
}

Optional<CSS::GridFitContent> Parser::parse_grid_fit_content(Vector<ComponentValue> const& component_values)
{
    // https://www.w3.org/TR/css-grid-2/#valdef-grid-template-columns-fit-content
    // 'fit-content( <length-percentage> )'
    // Represents the formula max(minimum, min(limit, max-content)), where minimum represents an auto minimum (which is often, but not always,
    // equal to a min-content minimum), and limit is the track sizing function passed as an argument to fit-content().
    // This is essentially calculated as the smaller of minmax(auto, max-content) and minmax(auto, limit).
    auto function_tokens = TokenStream(component_values);
    function_tokens.discard_whitespace();
    auto maybe_length_percentage = parse_length_percentage(function_tokens);
    if (maybe_length_percentage.has_value())
        return CSS::GridFitContent(CSS::GridSize(CSS::GridSize::Type::FitContent, maybe_length_percentage.value()));
    return {};
}

Optional<CSS::GridMinMax> Parser::parse_min_max(Vector<ComponentValue> const& component_values)
{
    // https://www.w3.org/TR/css-grid-2/#valdef-grid-template-columns-minmax
    // 'minmax(min, max)'
    // Defines a size range greater than or equal to min and less than or equal to max. If the max is
    // less than the min, then the max will be floored by the min (essentially yielding minmax(min,
    // min)). As a maximum, a <flex> value sets the track’s flex factor; it is invalid as a minimum.
    auto function_tokens = TokenStream(component_values);
    auto comma_separated_list = parse_a_comma_separated_list_of_component_values(function_tokens);
    if (comma_separated_list.size() != 2)
        return {};

    TokenStream part_one_tokens { comma_separated_list[0] };
    part_one_tokens.discard_whitespace();
    if (!part_one_tokens.has_next_token())
        return {};
    NonnullRawPtr<ComponentValue const> current_token = part_one_tokens.consume_a_token();
    auto min_grid_size = parse_grid_size(current_token);

    TokenStream part_two_tokens { comma_separated_list[1] };
    part_two_tokens.discard_whitespace();
    if (!part_two_tokens.has_next_token())
        return {};
    current_token = part_two_tokens.consume_a_token();
    auto max_grid_size = parse_grid_size(current_token);

    if (min_grid_size.has_value() && max_grid_size.has_value()) {
        // https://www.w3.org/TR/css-grid-2/#valdef-grid-template-columns-minmax
        // As a maximum, a <flex> value sets the track’s flex factor; it is invalid as a minimum.
        if (min_grid_size.value().is_flexible_length())
            return {};
        return CSS::GridMinMax(min_grid_size.value(), max_grid_size.value());
    }
    return {};
}

Optional<CSS::GridRepeat> Parser::parse_repeat(Vector<ComponentValue> const& component_values)
{
    // https://www.w3.org/TR/css-grid-2/#repeat-syntax
    // 7.2.3.1. Syntax of repeat()
    // The generic form of the repeat() syntax is, approximately,
    // repeat( [ <integer [1,∞]> | auto-fill | auto-fit ] , <track-list> )
    auto is_auto_fill = false;
    auto is_auto_fit = false;
    auto function_tokens = TokenStream(component_values);
    auto comma_separated_list = parse_a_comma_separated_list_of_component_values(function_tokens);
    if (comma_separated_list.size() != 2)
        return {};
    // The first argument specifies the number of repetitions.
    TokenStream part_one_tokens { comma_separated_list[0] };
    part_one_tokens.discard_whitespace();
    if (!part_one_tokens.has_next_token())
        return {};
    auto& current_token = part_one_tokens.consume_a_token();

    auto repeat_count = 0;
    if (current_token.is(Token::Type::Number) && current_token.token().number().is_integer() && current_token.token().number_value() > 0)
        repeat_count = current_token.token().number_value();
    else if (current_token.is_ident("auto-fill"sv))
        is_auto_fill = true;
    else if (current_token.is_ident("auto-fit"sv))
        is_auto_fit = true;

    // The second argument is a track list, which is repeated that number of times.
    TokenStream part_two_tokens { comma_separated_list[1] };
    part_two_tokens.discard_whitespace();
    if (!part_two_tokens.has_next_token())
        return {};

    Vector<Variant<ExplicitGridTrack, GridLineNames>> repeat_params;
    auto last_object_was_line_names = false;
    while (part_two_tokens.has_next_token()) {
        auto const& token = part_two_tokens.consume_a_token();
        Vector<String> line_names;
        if (token.is_block()) {
            if (last_object_was_line_names)
                return {};
            last_object_was_line_names = true;
            if (!token.block().is_square())
                return {};
            TokenStream block_tokens { token.block().value };
            while (block_tokens.has_next_token()) {
                auto const& current_block_token = block_tokens.consume_a_token();
                line_names.append(current_block_token.token().ident().to_string());
                block_tokens.discard_whitespace();
            }
            repeat_params.append(GridLineNames { move(line_names) });
            part_two_tokens.discard_whitespace();
        } else {
            last_object_was_line_names = false;
            auto track_sizing_function = parse_track_sizing_function(token);
            if (!track_sizing_function.has_value())
                return {};
            // However, there are some restrictions:
            // The repeat() notation can’t be nested.
            if (track_sizing_function.value().is_repeat())
                return {};

            // Automatic repetitions (auto-fill or auto-fit) cannot be combined with intrinsic or flexible sizes.
            // Note that 'auto' is also an intrinsic size (and thus not permitted) but we can't use
            // track_sizing_function.is_auto(..) to check for it, as it requires AvailableSize, which is why there is
            // a separate check for it below.
            // https://www.w3.org/TR/css-grid-2/#repeat-syntax
            // https://www.w3.org/TR/css-grid-2/#intrinsic-sizing-function
            if (track_sizing_function.value().is_default()
                && (track_sizing_function.value().grid_size().is_flexible_length() || token.is_ident("auto"sv))
                && (is_auto_fill || is_auto_fit))
                return {};
            if ((is_auto_fill || is_auto_fit) && track_sizing_function->is_minmax()) {
                auto const& minmax = track_sizing_function->minmax();
                if (!minmax.min_grid_size().is_definite() && !minmax.max_grid_size().is_definite()) {
                    return {};
                }
            }

            repeat_params.append(track_sizing_function.value());
            part_two_tokens.discard_whitespace();
        }
    }

    // Thus the precise syntax of the repeat() notation has several forms:
    // <track-repeat> = repeat( [ <integer [1,∞]> ] , [ <line-names>? <track-size> ]+ <line-names>? )
    // <auto-repeat>  = repeat( [ auto-fill | auto-fit ] , [ <line-names>? <fixed-size> ]+ <line-names>? )
    // <fixed-repeat> = repeat( [ <integer [1,∞]> ] , [ <line-names>? <fixed-size> ]+ <line-names>? )
    // <name-repeat>  = repeat( [ <integer [1,∞]> | auto-fill ], <line-names>+)

    // The <track-repeat> variant can represent the repetition of any <track-size>, but is limited to a
    // fixed number of repetitions.

    // The <auto-repeat> variant can repeat automatically to fill a space, but requires definite track
    // sizes so that the number of repetitions can be calculated. It can only appear once in the track
    // list, but the same track list can also contain <fixed-repeat>s.

    // The <name-repeat> variant is for adding line names to subgrids. It can only be used with the
    // subgrid keyword and cannot specify track sizes, only line names.

    // If a repeat() function that is not a <name-repeat> ends up placing two <line-names> adjacent to
    // each other, the name lists are merged. For example, repeat(2, [a] 1fr [b]) is equivalent to [a]
    // 1fr [b a] 1fr [b].
    if (is_auto_fill)
        return GridRepeat(GridTrackSizeList(move(repeat_params)), GridRepeat::Type::AutoFill);
    else if (is_auto_fit)
        return GridRepeat(GridTrackSizeList(move(repeat_params)), GridRepeat::Type::AutoFit);
    else
        return GridRepeat(GridTrackSizeList(move(repeat_params)), repeat_count);
}

Optional<CSS::ExplicitGridTrack> Parser::parse_track_sizing_function(ComponentValue const& token)
{
    if (token.is_function()) {
        auto const& function_token = token.function();
        auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.name });

        if (function_token.name.equals_ignoring_ascii_case("repeat"sv)) {
            auto maybe_repeat = parse_repeat(function_token.value);
            if (maybe_repeat.has_value())
                return CSS::ExplicitGridTrack(maybe_repeat.value());
            else
                return {};
        } else if (function_token.name.equals_ignoring_ascii_case("minmax"sv)) {
            auto maybe_min_max_value = parse_min_max(function_token.value);
            if (maybe_min_max_value.has_value())
                return CSS::ExplicitGridTrack(maybe_min_max_value.value());
            else
                return {};
        } else if (function_token.name.equals_ignoring_ascii_case("fit-content"sv)) {
            auto maybe_fit_content_value = parse_grid_fit_content(function_token.value);
            if (maybe_fit_content_value.has_value())
                return CSS::ExplicitGridTrack(maybe_fit_content_value.value());
            return {};
        } else if (auto maybe_calculated = parse_calculated_value(token)) {
            if (maybe_calculated->is_length())
                return ExplicitGridTrack(GridSize(maybe_calculated->as_length().length()));
            if (maybe_calculated->is_percentage())
                return ExplicitGridTrack(GridSize(maybe_calculated->as_percentage().percentage()));
            if (maybe_calculated->is_calculated() && maybe_calculated->as_calculated().resolves_to_length_percentage())
                return ExplicitGridTrack(GridSize(LengthPercentage(maybe_calculated->as_calculated())));
        }
        return {};
    } else if (token.is_ident("auto"sv)) {
        return CSS::ExplicitGridTrack(GridSize(Length::make_auto()));
    } else if (token.is_block()) {
        return {};
    } else {
        auto grid_size = parse_grid_size(token);
        if (!grid_size.has_value())
            return {};
        return CSS::ExplicitGridTrack(grid_size.value());
    }
}

RefPtr<GridTrackPlacementStyleValue> Parser::parse_grid_track_placement(TokenStream<ComponentValue>& tokens)
{
    // FIXME: This shouldn't be needed. Right now, the below code returns a CSSStyleValue even if no tokens are consumed!
    if (!tokens.has_next_token())
        return nullptr;

    if (tokens.remaining_token_count() > 3)
        return nullptr;

    // https://www.w3.org/TR/css-grid-2/#line-placement
    // Line-based Placement: the grid-row-start, grid-column-start, grid-row-end, and grid-column-end properties
    // <grid-line> =
    //     auto |
    //     <custom-ident> |
    //     [ <integer> && <custom-ident>? ] |
    //     [ span && [ <integer> || <custom-ident> ] ]
    auto is_valid_integer = [](auto& token) -> bool {
        // An <integer> value of zero makes the declaration invalid.
        if (token.is(Token::Type::Number) && token.token().number().is_integer() && token.token().number_value() != 0)
            return true;
        return false;
    };
    auto parse_custom_ident = [this](auto& tokens) {
        // The <custom-ident> additionally excludes the keywords span and auto.
        return parse_custom_ident_value(tokens, { { "span"sv, "auto"sv } });
    };

    auto transaction = tokens.begin_transaction();

    // FIXME: Handle the single-token case inside the loop instead, so that we can more easily call this from
    //        `parse_grid_area_shorthand_value()` using a single TokenStream.
    if (tokens.remaining_token_count() == 1) {
        if (auto custom_ident = parse_custom_ident(tokens)) {
            transaction.commit();
            return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line({}, custom_ident->custom_ident().to_string()));
        }
        auto const& token = tokens.consume_a_token();
        if (auto maybe_calculated = parse_calculated_value(token)) {
            if (maybe_calculated->is_number()) {
                transaction.commit();
                return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line(static_cast<int>(maybe_calculated->as_number().number()), {}));
            }
            if (maybe_calculated->is_calculated() && maybe_calculated->as_calculated().resolves_to_number()) {
                transaction.commit();
                return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line(static_cast<int>(maybe_calculated->as_calculated().resolve_integer({}).value()), {}));
            }
        }
        if (token.is_ident("auto"sv)) {
            transaction.commit();
            return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_auto());
        }
        if (is_valid_integer(token)) {
            transaction.commit();
            return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line(static_cast<int>(token.token().number_value()), {}));
        }
        return nullptr;
    }

    auto span_value = false;
    auto span_or_position_value = 0;
    String identifier_value;
    while (tokens.has_next_token()) {
        auto const& token = tokens.next_token();
        if (token.is_ident("auto"sv))
            return nullptr;
        if (token.is_ident("span"sv)) {
            if (span_value)
                return nullptr;
            tokens.discard_a_token(); // span
            if (tokens.has_next_token() && ((span_or_position_value != 0 && identifier_value.is_empty()) || (span_or_position_value == 0 && !identifier_value.is_empty())))
                return nullptr;
            span_value = true;
            continue;
        }
        if (is_valid_integer(token)) {
            if (span_or_position_value != 0)
                return nullptr;
            span_or_position_value = static_cast<int>(tokens.consume_a_token().token().to_integer());
            continue;
        }
        if (auto custom_ident = parse_custom_ident(tokens)) {
            if (!identifier_value.is_empty())
                return nullptr;
            identifier_value = custom_ident->custom_ident().to_string();
            continue;
        }
        break;
    }

    if (tokens.has_next_token())
        return nullptr;

    // Negative integers or zero are invalid.
    if (span_value && span_or_position_value < 1)
        return nullptr;

    // If the <integer> is omitted, it defaults to 1.
    if (span_or_position_value == 0)
        span_or_position_value = 1;

    transaction.commit();
    if (!identifier_value.is_empty())
        return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line(span_or_position_value, identifier_value));
    return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_span(span_or_position_value));
}

RefPtr<CSSStyleValue> Parser::parse_calculated_value(ComponentValue const& component_value)
{
    if (!component_value.is_function())
        return nullptr;

    auto const& function = component_value.function();

    CalculationContext context {};
    for (auto const& value_context : m_value_context.in_reverse()) {
        auto maybe_context = value_context.visit(
            [](PropertyID property_id) -> Optional<CalculationContext> {
                return CalculationContext {
                    .percentages_resolve_as = property_resolves_percentages_relative_to(property_id),
                    .resolve_numbers_as_integers = property_accepts_type(property_id, ValueType::Integer),
                };
            },
            [](FunctionContext const& function) -> Optional<CalculationContext> {
                // Gradients resolve percentages as lengths relative to the gradient-box.
                if (function.name.is_one_of_ignoring_ascii_case(
                        "linear-gradient"sv, "repeating-linear-gradient"sv,
                        "radial-gradient"sv, "repeating-radial-gradient"sv,
                        "conic-gradient"sv, "repeating-conic-gradient"sv)) {
                    return CalculationContext { .percentages_resolve_as = ValueType::Length };
                }
                // https://drafts.csswg.org/css-transforms-2/#transform-functions
                // The scale family of functions treats percentages as numbers.
                if (function.name.is_one_of_ignoring_ascii_case(
                        "scale"sv, "scalex"sv, "scaley"sv, "scalez"sv, "scale3d"sv)) {
                    return CalculationContext { .percentages_resolve_as = ValueType::Number };
                }
                // FIXME: Add other functions that provide a context for resolving values
                return {};
            });
        if (maybe_context.has_value()) {
            context = maybe_context.release_value();
            break;
        }
    }

    auto function_node = parse_a_calc_function_node(function, context);
    if (!function_node)
        return nullptr;

    auto function_type = function_node->numeric_type();
    if (!function_type.has_value())
        return nullptr;

    return CalculatedStyleValue::create(function_node.release_nonnull(), function_type.release_value(), context);
}

RefPtr<CalculationNode> Parser::parse_a_calc_function_node(Function const& function, CalculationContext const& context)
{
    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function.name });

    if (function.name.equals_ignoring_ascii_case("calc"sv))
        return parse_a_calculation(function.value, context);

    if (auto maybe_function = parse_math_function(function, context))
        return maybe_function;

    return nullptr;
}

RefPtr<CalculationNode> Parser::convert_to_calculation_node(CalcParsing::Node const& node, CalculationContext const& context)
{
    return node.visit(
        [this, &context](NonnullOwnPtr<CalcParsing::ProductNode> const& product_node) -> RefPtr<CalculationNode> {
            Vector<NonnullRefPtr<CalculationNode>> children;
            children.ensure_capacity(product_node->children.size());

            for (auto const& child : product_node->children) {
                if (auto child_as_node = convert_to_calculation_node(child, context)) {
                    children.append(child_as_node.release_nonnull());
                } else {
                    return nullptr;
                }
            }

            return ProductCalculationNode::create(move(children));
        },
        [this, &context](NonnullOwnPtr<CalcParsing::SumNode> const& sum_node) -> RefPtr<CalculationNode> {
            Vector<NonnullRefPtr<CalculationNode>> children;
            children.ensure_capacity(sum_node->children.size());

            for (auto const& child : sum_node->children) {
                if (auto child_as_node = convert_to_calculation_node(child, context)) {
                    children.append(child_as_node.release_nonnull());
                } else {
                    return nullptr;
                }
            }

            return SumCalculationNode::create(move(children));
        },
        [this, &context](NonnullOwnPtr<CalcParsing::InvertNode> const& invert_node) -> RefPtr<CalculationNode> {
            if (auto child_as_node = convert_to_calculation_node(invert_node->child, context))
                return InvertCalculationNode::create(child_as_node.release_nonnull());
            return nullptr;
        },
        [this, &context](NonnullOwnPtr<CalcParsing::NegateNode> const& negate_node) -> RefPtr<CalculationNode> {
            if (auto child_as_node = convert_to_calculation_node(negate_node->child, context))
                return NegateCalculationNode::create(child_as_node.release_nonnull());
            return nullptr;
        },
        [this, &context](NonnullRawPtr<ComponentValue const> const& component_value) -> RefPtr<CalculationNode> {
            // NOTE: This is the "process the leaf nodes" part of step 5 of https://drafts.csswg.org/css-values-4/#parse-a-calculation
            //       We divert a little from the spec: Rather than modify an existing tree of values, we construct a new one from that source tree.
            //       This lets us make CalculationNodes immutable.

            // 1. If leaf is a parenthesized simple block, replace leaf with the result of parsing a calculation from leaf’s contents.
            if (component_value->is_block() && component_value->block().is_paren()) {
                auto leaf_calculation = parse_a_calculation(component_value->block().value, context);
                if (!leaf_calculation)
                    return nullptr;

                return leaf_calculation.release_nonnull();
            }

            // 2. If leaf is a math function, replace leaf with the internal representation of that math function.
            // NOTE: All function tokens at this point should be math functions.
            if (component_value->is_function()) {
                auto const& function = component_value->function();
                auto leaf_calculation = parse_a_calc_function_node(function, context);
                if (!leaf_calculation)
                    return nullptr;

                return leaf_calculation.release_nonnull();
            }

            // AD-HOC: We also need to convert tokens into their numeric types.

            if (component_value->is(Token::Type::Ident)) {
                auto maybe_keyword = keyword_from_string(component_value->token().ident());
                if (!maybe_keyword.has_value())
                    return nullptr;
                return NumericCalculationNode::from_keyword(*maybe_keyword, context);
            }

            if (component_value->is(Token::Type::Number))
                return NumericCalculationNode::create(component_value->token().number(), context);

            if (component_value->is(Token::Type::Dimension)) {
                auto numeric_value = component_value->token().dimension_value();
                auto unit_string = component_value->token().dimension_unit();

                if (auto length_type = Length::unit_from_name(unit_string); length_type.has_value())
                    return NumericCalculationNode::create(Length { numeric_value, length_type.release_value() }, context);

                if (auto angle_type = Angle::unit_from_name(unit_string); angle_type.has_value())
                    return NumericCalculationNode::create(Angle { numeric_value, angle_type.release_value() }, context);

                if (auto flex_type = Flex::unit_from_name(unit_string); flex_type.has_value()) {
                    // https://www.w3.org/TR/css3-grid-layout/#fr-unit
                    // NOTE: <flex> values are not <length>s (nor are they compatible with <length>s, like some <percentage> values),
                    //       so they cannot be represented in or combined with other unit types in calc() expressions.
                    // FIXME: Flex is allowed in calc(), so figure out what this spec text means and how to implement it.
                    dbgln_if(CSS_PARSER_DEBUG, "Rejecting <flex> in calc()");
                    return nullptr;
                }

                if (auto frequency_type = Frequency::unit_from_name(unit_string); frequency_type.has_value())
                    return NumericCalculationNode::create(Frequency { numeric_value, frequency_type.release_value() }, context);

                if (auto resolution_type = Resolution::unit_from_name(unit_string); resolution_type.has_value())
                    return NumericCalculationNode::create(Resolution { numeric_value, resolution_type.release_value() }, context);

                if (auto time_type = Time::unit_from_name(unit_string); time_type.has_value())
                    return NumericCalculationNode::create(Time { numeric_value, time_type.release_value() }, context);

                dbgln_if(CSS_PARSER_DEBUG, "Unrecognized dimension type in calc() expression: {}", component_value->to_string());
                return nullptr;
            }

            if (component_value->is(Token::Type::Percentage))
                return NumericCalculationNode::create(Percentage { component_value->token().percentage() }, context);

            // NOTE: If we get here, then we have a ComponentValue that didn't get replaced with something else,
            //       so the calc() is invalid.
            dbgln_if(CSS_PARSER_DEBUG, "Leftover ComponentValue in calc tree! That probably means the syntax is invalid, but maybe we just didn't implement `{}` yet.", component_value->to_debug_string());
            return nullptr;
        },
        [](CalcParsing::Operator const& op) -> RefPtr<CalculationNode> {
            dbgln_if(CSS_PARSER_DEBUG, "Leftover Operator {} in calc tree!", op.delim);
            return nullptr;
        });
}

// https://drafts.csswg.org/css-values-4/#parse-a-calculation
RefPtr<CalculationNode> Parser::parse_a_calculation(Vector<ComponentValue> const& original_values, CalculationContext const& context)
{
    // 1. Discard any <whitespace-token>s from values.
    // 2. An item in values is an “operator” if it’s a <delim-token> with the value "+", "-", "*", or "/". Otherwise, it’s a “value”.

    Vector<CalcParsing::Node> values;
    for (auto const& value : original_values) {
        if (value.is(Token::Type::Whitespace))
            continue;
        if (value.is(Token::Type::Delim)) {
            if (first_is_one_of(value.token().delim(), static_cast<u32>('+'), static_cast<u32>('-'), static_cast<u32>('*'), static_cast<u32>('/'))) {
                // NOTE: Sequential operators are invalid syntax.
                if (!values.is_empty() && values.last().has<CalcParsing::Operator>())
                    return nullptr;

                values.append(CalcParsing::Operator { static_cast<char>(value.token().delim()) });
                continue;
            }
        }

        values.append(NonnullRawPtr { value });
    }

    // If we have no values, the syntax is invalid.
    if (values.is_empty())
        return nullptr;

    // NOTE: If the first or last value is an operator, the syntax is invalid.
    if (values.first().has<CalcParsing::Operator>() || values.last().has<CalcParsing::Operator>())
        return nullptr;

    // 3. Collect children into Product and Invert nodes.
    //    For every consecutive run of value items in values separated by "*" or "/" operators:
    while (true) {
        Optional<size_t> first_product_operator = values.find_first_index_if([](auto const& item) {
            return item.template has<CalcParsing::Operator>()
                && first_is_one_of(item.template get<CalcParsing::Operator>().delim, '*', '/');
        });

        if (!first_product_operator.has_value())
            break;

        auto start_of_run = first_product_operator.value() - 1;
        auto end_of_run = first_product_operator.value() + 1;
        for (auto i = start_of_run + 1; i < values.size(); i += 2) {
            auto& item = values[i];
            if (!item.has<CalcParsing::Operator>()) {
                end_of_run = i - 1;
                break;
            }

            auto delim = item.get<CalcParsing::Operator>().delim;
            if (!first_is_one_of(delim, '*', '/')) {
                end_of_run = i - 1;
                break;
            }
        }

        // 1. For each "/" operator in the run, replace its right-hand value item rhs with an Invert node containing rhs as its child.
        Vector<CalcParsing::Node> run_values;
        run_values.append(move(values[start_of_run]));
        for (auto i = start_of_run + 1; i <= end_of_run; i += 2) {
            auto& operator_ = values[i].get<CalcParsing::Operator>().delim;
            auto& rhs = values[i + 1];
            if (operator_ == '/') {
                run_values.append(make<CalcParsing::InvertNode>(move(rhs)));
                continue;
            }
            VERIFY(operator_ == '*');
            run_values.append(move(rhs));
        }
        // 2. Replace the entire run with a Product node containing the value items of the run as its children.
        values.remove(start_of_run, end_of_run - start_of_run + 1);
        values.insert(start_of_run, make<CalcParsing::ProductNode>(move(run_values)));
    }

    // 4. Collect children into Sum and Negate nodes.
    Optional<CalcParsing::Node> single_value;
    {
        // 1. For each "-" operator item in values, replace its right-hand value item rhs with a Negate node containing rhs as its child.
        for (auto i = 0u; i < values.size(); ++i) {
            auto& maybe_minus_operator = values[i];
            if (!maybe_minus_operator.has<CalcParsing::Operator>() || maybe_minus_operator.get<CalcParsing::Operator>().delim != '-')
                continue;

            auto rhs_index = ++i;
            auto negate_node = make<CalcParsing::NegateNode>(move(values[rhs_index]));
            values.remove(rhs_index);
            values.insert(rhs_index, move(negate_node));
        }

        // 2. If values has only one item, and it is a Product node or a parenthesized simple block, replace values with that item.
        if (values.size() == 1) {
            values.first().visit(
                [&](ComponentValue const& component_value) {
                    if (component_value.is_block() && component_value.block().is_paren())
                        single_value = NonnullRawPtr { component_value };
                },
                [&](NonnullOwnPtr<CalcParsing::ProductNode>& node) {
                    single_value = move(node);
                },
                [](auto&) {});
        }
        //    Otherwise, replace values with a Sum node containing the value items of values as its children.
        if (!single_value.has_value()) {
            values.remove_all_matching([](CalcParsing::Node& value) { return value.has<CalcParsing::Operator>(); });
            single_value = make<CalcParsing::SumNode>(move(values));
        }
    }
    VERIFY(single_value.has_value());

    // 5. At this point values is a tree of Sum, Product, Negate, and Invert nodes, with other types of values at the leaf nodes. Process the leaf nodes.
    // NOTE: We process leaf nodes as part of this conversion.
    auto calculation_tree = convert_to_calculation_node(*single_value, context);
    if (!calculation_tree)
        return nullptr;

    // 6. Return the result of simplifying a calculation tree from values.
    return simplify_a_calculation_tree(*calculation_tree, context, CalculationResolutionContext {});
}

// https://drafts.csswg.org/css-fonts/#typedef-opentype-tag
RefPtr<StringStyleValue> Parser::parse_opentype_tag_value(TokenStream<ComponentValue>& tokens)
{
    // <opentype-tag> = <string>
    // The <opentype-tag> is a case-sensitive OpenType feature tag.
    // As specified in the OpenType specification [OPENTYPE], feature tags contain four ASCII characters.
    // Tag strings longer or shorter than four characters, or containing characters outside the U+20–7E codepoint range are invalid.

    auto transaction = tokens.begin_transaction();
    auto string_value = parse_string_value(tokens);
    if (string_value == nullptr)
        return nullptr;

    auto string = string_value->string_value().bytes_as_string_view();
    if (string.length() != 4)
        return nullptr;
    for (char c : string) {
        if (c < 0x20 || c > 0x7E)
            return nullptr;
    }

    transaction.commit();
    return string_value;
}

NonnullRefPtr<CSSStyleValue> Parser::resolve_unresolved_style_value(ParsingParams const& context, DOM::Element& element, Optional<PseudoElement> pseudo_element, PropertyID property_id, UnresolvedStyleValue const& unresolved)
{
    // Unresolved always contains a var() or attr(), unless it is a custom property's value, in which case we shouldn't be trying
    // to produce a different CSSStyleValue from it.
    VERIFY(unresolved.contains_var_or_attr());

    // If the value is invalid, we fall back to `unset`: https://www.w3.org/TR/css-variables-1/#invalid-at-computed-value-time

    auto parser = Parser::create(context, ""sv);
    return parser.resolve_unresolved_style_value(element, pseudo_element, property_id, unresolved);
}

class PropertyDependencyNode : public RefCounted<PropertyDependencyNode> {
public:
    static NonnullRefPtr<PropertyDependencyNode> create(FlyString name)
    {
        return adopt_ref(*new PropertyDependencyNode(move(name)));
    }

    void add_child(NonnullRefPtr<PropertyDependencyNode> new_child)
    {
        for (auto const& child : m_children) {
            if (child->m_name == new_child->m_name)
                return;
        }

        // We detect self-reference already.
        VERIFY(new_child->m_name != m_name);
        m_children.append(move(new_child));
    }

    bool has_cycles()
    {
        if (m_marked)
            return true;

        TemporaryChange change { m_marked, true };
        for (auto& child : m_children) {
            if (child->has_cycles())
                return true;
        }
        return false;
    }

private:
    explicit PropertyDependencyNode(FlyString name)
        : m_name(move(name))
    {
    }

    FlyString m_name;
    Vector<NonnullRefPtr<PropertyDependencyNode>> m_children;
    bool m_marked { false };
};

NonnullRefPtr<CSSStyleValue> Parser::resolve_unresolved_style_value(DOM::Element& element, Optional<PseudoElement> pseudo_element, PropertyID property_id, UnresolvedStyleValue const& unresolved)
{
    TokenStream unresolved_values_without_variables_expanded { unresolved.values() };
    Vector<ComponentValue> values_with_variables_expanded;

    HashMap<FlyString, NonnullRefPtr<PropertyDependencyNode>> dependencies;
    ScopeGuard mark_element_if_uses_custom_properties = [&] {
        for (auto const& name : dependencies.keys()) {
            if (is_a_custom_property_name_string(name)) {
                element.set_style_uses_css_custom_properties(true);
                return;
            }
        }
    };
    if (!expand_variables(element, pseudo_element, string_from_property_id(property_id), dependencies, unresolved_values_without_variables_expanded, values_with_variables_expanded))
        return CSSKeywordValue::create(Keyword::Unset);

    TokenStream unresolved_values_with_variables_expanded { values_with_variables_expanded };
    Vector<ComponentValue> expanded_values;
    if (!expand_unresolved_values(element, string_from_property_id(property_id), unresolved_values_with_variables_expanded, expanded_values))
        return CSSKeywordValue::create(Keyword::Unset);

    auto expanded_value_tokens = TokenStream { expanded_values };
    if (auto parsed_value = parse_css_value(property_id, expanded_value_tokens); !parsed_value.is_error())
        return parsed_value.release_value();

    return CSSKeywordValue::create(Keyword::Unset);
}

static RefPtr<CSSStyleValue const> get_custom_property(DOM::Element const& element, Optional<CSS::PseudoElement> pseudo_element, FlyString const& custom_property_name)
{
    if (pseudo_element.has_value()) {
        if (auto it = element.custom_properties(pseudo_element).find(custom_property_name); it != element.custom_properties(pseudo_element).end())
            return it->value.value;
    }

    for (auto const* current_element = &element; current_element; current_element = current_element->parent_or_shadow_host_element()) {
        if (auto it = current_element->custom_properties({}).find(custom_property_name); it != current_element->custom_properties({}).end())
            return it->value.value;
    }
    return nullptr;
}

bool Parser::expand_variables(DOM::Element& element, Optional<PseudoElement> pseudo_element, FlyString const& property_name, HashMap<FlyString, NonnullRefPtr<PropertyDependencyNode>>& dependencies, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest)
{
    // Arbitrary large value chosen to avoid the billion-laughs attack.
    // https://www.w3.org/TR/css-variables-1/#long-variables
    size_t const MAX_VALUE_COUNT = 16384;
    if (source.remaining_token_count() + dest.size() > MAX_VALUE_COUNT) {
        dbgln("Stopped expanding CSS variables: maximum length reached.");
        return false;
    }

    auto get_dependency_node = [&](FlyString const& name) -> NonnullRefPtr<PropertyDependencyNode> {
        if (auto existing = dependencies.get(name); existing.has_value())
            return *existing.value();
        auto new_node = PropertyDependencyNode::create(name);
        dependencies.set(name, new_node);
        return new_node;
    };

    while (source.has_next_token()) {
        auto const& value = source.consume_a_token();
        if (value.is_block()) {
            auto const& source_block = value.block();
            Vector<ComponentValue> block_values;
            TokenStream source_block_contents { source_block.value };
            if (!expand_variables(element, pseudo_element, property_name, dependencies, source_block_contents, block_values))
                return false;
            dest.empend(SimpleBlock { source_block.token, move(block_values) });
            continue;
        }
        if (!value.is_function()) {
            dest.empend(value.token());
            continue;
        }
        if (!value.function().name.equals_ignoring_ascii_case("var"sv)) {
            auto const& source_function = value.function();
            Vector<ComponentValue> function_values;
            TokenStream source_function_contents { source_function.value };
            if (!expand_variables(element, pseudo_element, property_name, dependencies, source_function_contents, function_values))
                return false;
            dest.empend(Function { source_function.name, move(function_values) });
            continue;
        }

        TokenStream var_contents { value.function().value };
        var_contents.discard_whitespace();
        if (!var_contents.has_next_token())
            return false;

        auto const& custom_property_name_token = var_contents.consume_a_token();
        if (!custom_property_name_token.is(Token::Type::Ident))
            return false;
        auto custom_property_name = custom_property_name_token.token().ident();
        if (!custom_property_name.bytes_as_string_view().starts_with("--"sv))
            return false;

        // Detect dependency cycles. https://www.w3.org/TR/css-variables-1/#cycles
        // We do not do this by the spec, since we are not keeping a graph of var dependencies around,
        // but rebuilding it every time.
        if (custom_property_name == property_name)
            return false;
        auto parent = get_dependency_node(property_name);
        auto child = get_dependency_node(custom_property_name);
        parent->add_child(child);
        if (parent->has_cycles())
            return false;

        if (auto custom_property_value = get_custom_property(element, pseudo_element, custom_property_name)) {
            VERIFY(custom_property_value->is_unresolved());
            TokenStream custom_property_tokens { custom_property_value->as_unresolved().values() };

            auto dest_size_before = dest.size();
            if (!expand_variables(element, pseudo_element, custom_property_name, dependencies, custom_property_tokens, dest))
                return false;

            // If the size of dest has increased, then the custom property is not the initial guaranteed-invalid value.
            // If it hasn't increased, then it is the initial guaranteed-invalid value, and thus we should move on to the fallback value.
            if (dest_size_before < dest.size())
                continue;

            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Expanding custom property '{}' did not return any tokens, treating it as invalid and moving on to the fallback value.", custom_property_name);
        }

        // Use the provided fallback value, if any.
        var_contents.discard_whitespace();
        if (var_contents.has_next_token()) {
            auto const& comma_token = var_contents.consume_a_token();
            if (!comma_token.is(Token::Type::Comma))
                return false;
            var_contents.discard_whitespace();
            if (!expand_variables(element, pseudo_element, property_name, dependencies, var_contents, dest))
                return false;
        }
    }
    return true;
}

bool Parser::expand_unresolved_values(DOM::Element& element, FlyString const& property_name, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest)
{
    while (source.has_next_token()) {
        auto const& value = source.consume_a_token();
        if (value.is_function()) {
            if (value.function().name.equals_ignoring_ascii_case("attr"sv)) {
                if (!substitute_attr_function(element, property_name, value.function(), dest))
                    return false;
                continue;
            }

            auto const& source_function = value.function();
            Vector<ComponentValue> function_values;
            TokenStream source_function_contents { source_function.value };
            if (!expand_unresolved_values(element, property_name, source_function_contents, function_values))
                return false;
            dest.empend(Function { source_function.name, move(function_values) });
            continue;
        }
        if (value.is_block()) {
            auto const& source_block = value.block();
            TokenStream source_block_values { source_block.value };
            Vector<ComponentValue> block_values;
            if (!expand_unresolved_values(element, property_name, source_block_values, block_values))
                return false;
            dest.empend(SimpleBlock { source_block.token, move(block_values) });
            continue;
        }
        dest.empend(value.token());
    }

    return true;
}

// https://drafts.csswg.org/css-values-5/#attr-substitution
bool Parser::substitute_attr_function(DOM::Element& element, FlyString const& property_name, Function const& attr_function, Vector<ComponentValue>& dest)
{
    // First, parse the arguments to attr():
    // attr() = attr( <q-name> <attr-type>? , <declaration-value>?)
    // <attr-type> = string | url | ident | color | number | percentage | length | angle | time | frequency | flex | <dimension-unit>
    TokenStream attr_contents { attr_function.value };
    attr_contents.discard_whitespace();
    if (!attr_contents.has_next_token())
        return false;

    // - Attribute name
    // FIXME: Support optional attribute namespace
    if (!attr_contents.next_token().is(Token::Type::Ident))
        return false;
    auto attribute_name = attr_contents.consume_a_token().token().ident();
    attr_contents.discard_whitespace();

    // - Attribute type (optional)
    auto attribute_type = "string"_fly_string;
    if (attr_contents.next_token().is(Token::Type::Ident)) {
        attribute_type = attr_contents.consume_a_token().token().ident();
        attr_contents.discard_whitespace();
    }

    // - Comma, then fallback values (optional)
    bool has_fallback_values = false;
    if (attr_contents.has_next_token()) {
        if (!attr_contents.next_token().is(Token::Type::Comma))
            return false;
        (void)attr_contents.consume_a_token(); // Comma
        has_fallback_values = true;
    }

    // Then, run the substitution algorithm:

    // 1. If the attr() function has a substitution value, replace the attr() function by the substitution value.
    // https://drafts.csswg.org/css-values-5/#attr-types
    if (element.has_attribute(attribute_name)) {
        auto parse_string_as_component_value = [this](String const& string) {
            auto tokens = Tokenizer::tokenize(string, "utf-8"sv);
            TokenStream stream { tokens };
            return parse_a_component_value(stream);
        };

        auto attribute_value = element.get_attribute_value(attribute_name);
        if (attribute_type.equals_ignoring_ascii_case("angle"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = parse_string_as_component_value(attribute_value);
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Angle::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("color"_fly_string)) {
            // Parse a component value from the attribute’s value.
            // If the result is a <hex-color> or a named color ident, the substitution value is that result as a <color>.
            // Otherwise there is no substitution value.
            auto component_value = parse_string_as_component_value(attribute_value);
            if (component_value.has_value()) {
                if ((component_value->is(Token::Type::Hash)
                        && Color::from_string(MUST(String::formatted("#{}", component_value->token().hash_value()))).has_value())
                    || (component_value->is(Token::Type::Ident)
                        && Color::from_string(component_value->token().ident()).has_value())) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("flex"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = parse_string_as_component_value(attribute_value);
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Flex::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("frequency"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = parse_string_as_component_value(attribute_value);
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Frequency::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("ident"_fly_string)) {
            // The substitution value is a CSS <custom-ident>, whose value is the literal value of the attribute,
            // with leading and trailing ASCII whitespace stripped. (No CSS parsing of the value is performed.)
            // If the attribute value, after trimming, is the empty string, there is instead no substitution value.
            // If the <custom-ident>’s value is a CSS-wide keyword or `default`, there is instead no substitution value.
            auto substitution_value = MUST(attribute_value.trim(Infra::ASCII_WHITESPACE));
            if (!substitution_value.is_empty()
                && !substitution_value.equals_ignoring_ascii_case("default"sv)
                && !is_css_wide_keyword(substitution_value)) {
                dest.empend(Token::create_ident(substitution_value));
                return true;
            }
        } else if (attribute_type.equals_ignoring_ascii_case("length"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = parse_string_as_component_value(attribute_value);
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Length::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("number"_fly_string)) {
            // Parse a component value from the attribute’s value.
            // If the result is a <number-token>, the result is the substitution value.
            // Otherwise, there is no substitution value.
            auto component_value = parse_string_as_component_value(attribute_value);
            if (component_value.has_value() && component_value->is(Token::Type::Number)) {
                dest.append(component_value.release_value());
                return true;
            }
        } else if (attribute_type.equals_ignoring_ascii_case("percentage"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = parse_string_as_component_value(attribute_value);
            // If the result is a <percentage-token>, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Percentage)) {
                dest.append(component_value.release_value());
                return true;
            }
        } else if (attribute_type.equals_ignoring_ascii_case("string"_fly_string)) {
            // The substitution value is a CSS string, whose value is the literal value of the attribute.
            // (No CSS parsing or "cleanup" of the value is performed.)
            // No value triggers fallback.
            dest.empend(Token::create_string(attribute_value));
            return true;
        } else if (attribute_type.equals_ignoring_ascii_case("time"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = parse_string_as_component_value(attribute_value);
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Time::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("url"_fly_string)) {
            // The substitution value is a CSS <url> value, whose url is the literal value of the attribute.
            // (No CSS parsing or "cleanup" of the value is performed.)
            // No value triggers fallback.
            dest.empend(Token::create_url(attribute_value));
            return true;
        } else {
            // Dimension units
            // Parse a component value from the attribute’s value.
            // If the result is a <number-token>, the substitution value is a dimension with the result’s value, and the given unit.
            // Otherwise, there is no substitution value.
            auto component_value = parse_string_as_component_value(attribute_value);
            if (component_value.has_value() && component_value->is(Token::Type::Number)) {
                if (attribute_value == "%"sv) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto angle_unit = Angle::unit_from_name(attribute_type); angle_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto flex_unit = Flex::unit_from_name(attribute_type); flex_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto frequency_unit = Frequency::unit_from_name(attribute_type); frequency_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto length_unit = Length::unit_from_name(attribute_type); length_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto time_unit = Time::unit_from_name(attribute_type); time_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else {
                    // Not a dimension unit.
                    return false;
                }
            }
        }
    }

    // 2. Otherwise, if the attr() function has a fallback value as its last argument, replace the attr() function by the fallback value.
    //    If there are any var() or attr() references in the fallback, substitute them as well.
    if (has_fallback_values)
        return expand_unresolved_values(element, property_name, attr_contents, dest);

    if (attribute_type.equals_ignoring_ascii_case("string"_fly_string)) {
        // If the <attr-type> argument is string, defaults to the empty string if omitted
        dest.empend(Token::create_string({}));
        return true;
    }

    // 3. Otherwise, the property containing the attr() function is invalid at computed-value time.
    return false;
}

}

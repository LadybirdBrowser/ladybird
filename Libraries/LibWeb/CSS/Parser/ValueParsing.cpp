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
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/QuickSort.h>
#include <AK/StringConversions.h>
#include <AK/TemporaryChange.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/AnchorSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorMixStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/FitContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/HSLColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/HWBColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LCHLikeColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/LabLikeColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/LightDarkStyleValue.h>
#include <LibWeb/CSS/StyleValues/LinearGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RGBColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/RepeatStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::CSS::Parser {

RefPtr<StyleValue const> Parser::parse_comma_separated_value_list(TokenStream<ComponentValue>& tokens, ParseFunction parse_one_value)
{
    auto first = parse_one_value(tokens);
    if (!first || !tokens.has_next_token())
        return first;

    StyleValueVector values;
    values.append(first.release_nonnull());

    while (tokens.has_next_token()) {
        if (!tokens.consume_a_token().is(Token::Type::Comma))
            return nullptr;

        if (auto maybe_value = parse_one_value(tokens)) {
            values.append(maybe_value.release_nonnull());
            continue;
        }
        return nullptr;
    }

    return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
}

// https://drafts.csswg.org/css-syntax/#typedef-declaration-value
Optional<Vector<ComponentValue>> Parser::parse_declaration_value(TokenStream<ComponentValue>& tokens, StopAtComma stop_at_comma)
{
    // The <declaration-value> production matches any sequence of one or more tokens, so long as the sequence does not
    // contain <bad-string-token>, <bad-url-token>, unmatched <)-token>, <]-token>, or <}-token>, or top-level
    // <semicolon-token> tokens or <delim-token> tokens with a value of "!". It represents the entirety of what a valid
    // declaration can have as its value.
    auto transaction = tokens.begin_transaction();
    Vector<ComponentValue> declaration_value;
    while (tokens.has_next_token()) {
        auto const& peek = tokens.next_token();
        if (!peek.is_token()) {
            declaration_value.append(tokens.consume_a_token());
            continue;
        }

        bool valid = true;
        switch (peek.token().type()) {
        case Token::Type::Invalid:
        case Token::Type::EndOfFile:
        case Token::Type::BadString:
        case Token::Type::BadUrl:
        case Token::Type::Semicolon:
            // NB: We're dealing with ComponentValues, so all valid function and block-related tokens will already be
            //     converted to Function or SimpleBlock ComponentValues. Any remaining ones are invalid.
        case Token::Type::Function:
        case Token::Type::OpenCurly:
        case Token::Type::OpenParen:
        case Token::Type::OpenSquare:
        case Token::Type::CloseCurly:
        case Token::Type::CloseParen:
        case Token::Type::CloseSquare:
            valid = false;
            break;
        case Token::Type::Delim:
            valid = peek.token().delim() != '!';
            break;
        case Token::Type::Comma:
            valid = stop_at_comma == StopAtComma::No;
            break;
        default:
            break;
        }

        if (!valid)
            break;
        declaration_value.append(tokens.consume_a_token());
    }

    if (declaration_value.is_empty())
        return OptionalNone {};
    transaction.commit();
    return declaration_value;
}

Optional<Dimension> Parser::parse_dimension(ComponentValue const& component_value)
{
    if (component_value.is(Token::Type::Dimension)) {
        auto numeric_value = component_value.token().dimension_value();
        auto unit_string = component_value.token().dimension_unit();

        if (auto length_type = string_to_length_unit(unit_string); length_type.has_value())
            return Length { numeric_value, length_type.release_value() };

        if (auto angle_type = string_to_angle_unit(unit_string); angle_type.has_value())
            return Angle { numeric_value, angle_type.release_value() };

        if (auto flex_type = string_to_flex_unit(unit_string); flex_type.has_value())
            return Flex { numeric_value, flex_type.release_value() };

        if (auto frequency_type = string_to_frequency_unit(unit_string); frequency_type.has_value())
            return Frequency { numeric_value, frequency_type.release_value() };

        if (auto resolution_type = string_to_resolution_unit(unit_string); resolution_type.has_value())
            return Resolution { numeric_value, resolution_type.release_value() };

        if (auto time_type = string_to_time_unit(unit_string); time_type.has_value())
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
        // FIXME: Deal with ->is_anchor_size()
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
        // FIXME: Deal with ->is_anchor_size()
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
                return maybe_calc->as_number().number();
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
RefPtr<StyleValue const> Parser::parse_family_name_value(TokenStream<ComponentValue>& tokens)
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
            parts.append(ident.to_string());
            tokens.discard_whitespace();
            continue;
        }

        break;
    }

    if (parts.is_empty())
        return nullptr;

    if (parts.size() == 1) {
        // <generic-family> is a separate type from <family-name>, and so isn't allowed here.
        auto maybe_keyword = keyword_from_string(parts.first());
        if (is_css_wide_keyword(parts.first()) || parts.first().equals_ignoring_ascii_case("default"sv))
            return nullptr;
        if (maybe_keyword.has_value() && keyword_to_generic_font_family(maybe_keyword.value()).has_value())
            return nullptr;
    }

    auto complete_name = MUST(String::join(' ', parts));

    transaction.commit();
    return CustomIdentStyleValue::create(complete_name);
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
        ErrorReporter::the().report(InvalidValueError {
            .value_type = "<urange>"_fly_string,
            .value_string = tokens.dump_string(),
            .description = "Doesn't start with 'u'."_string,
        });
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

    ErrorReporter::the().report(InvalidValueError {
        .value_type = "<urange>"_fly_string,
        .value_string = tokens.dump_string(),
        .description = "Did not match grammar."_string,
    });
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
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<urange>"_fly_string,
                .value_string = MUST(String::from_utf8(text)),
                .description = MUST(String::formatted("end_value ({}) > maximum ({})", end_value, maximum_allowed_code_point)),
            });
            return {};
        }

        // 2. If start value is greater than end value, the <urange> is invalid and a syntax error.
        if (start_value > end_value) {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<urange>"_fly_string,
                .value_string = MUST(String::from_utf8(text)),
                .description = MUST(String::formatted("start_value ({}) > end_value ({})", start_value, end_value)),
            });
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
        ErrorReporter::the().report(InvalidValueError {
            .value_type = "<urange>"_fly_string,
            .value_string = MUST(String::from_utf8(text)),
            .description = MUST(String::formatted("Second character was '{}', expected '+'.", lexer.consume())),
        });
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
        ErrorReporter::the().report(InvalidValueError {
            .value_type = "<urange>"_fly_string,
            .value_string = MUST(String::from_utf8(text)),
            .description = MUST(String::formatted("Start value had {} digits/?s, expected between 1 and 6.", consumed_code_points)),
        });
        return {};
    }
    StringView start_value_code_points = text.substring_view(start_position, consumed_code_points);

    //    If any U+003F QUESTION MARK (?) code points were consumed, then:
    if (question_marks.length() > 0) {
        // 1. If there are any code points left in text, this is an invalid <urange>,
        //    and this algorithm must exit.
        if (lexer.tell_remaining() != 0) {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<urange>"_fly_string,
                .value_string = MUST(String::from_utf8(text)),
                .description = MUST(String::formatted("Has {} trailing unused code points.", lexer.tell_remaining())),
            });
            return {};
        }

        // 2. Interpret the consumed code points as a hexadecimal number,
        //    with the U+003F QUESTION MARK (?) code points replaced by U+0030 DIGIT ZERO (0) code points.
        //    This is the start value.
        auto start_value_string = start_value_code_points.replace("?"sv, "0"sv, ReplaceMode::All);
        auto maybe_start_value = AK::parse_hexadecimal_number<u32>(start_value_string);
        if (!maybe_start_value.has_value()) {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<urange>"_fly_string,
                .value_string = MUST(String::from_utf8(text)),
                .description = "?-converted start value did not parse as hex number."_string,
            });
            return {};
        }
        u32 start_value = maybe_start_value.release_value();

        // 3. Interpret the consumed code points as a hexadecimal number again,
        //    with the U+003F QUESTION MARK (?) code points replaced by U+0046 LATIN CAPITAL LETTER F (F) code points.
        //    This is the end value.
        auto end_value_string = start_value_code_points.replace("?"sv, "F"sv, ReplaceMode::All);
        auto maybe_end_value = AK::parse_hexadecimal_number<u32>(end_value_string);
        if (!maybe_end_value.has_value()) {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<urange>"_fly_string,
                .value_string = MUST(String::from_utf8(text)),
                .description = "?-converted end value did not parse as hex number."_string,
            });
            return {};
        }
        u32 end_value = maybe_end_value.release_value();

        // 4. Exit this algorithm.
        return make_valid_unicode_range(start_value, end_value);
    }
    //   Otherwise, interpret the consumed code points as a hexadecimal number. This is the start value.
    auto maybe_start_value = AK::parse_hexadecimal_number<u32>(start_value_code_points);
    if (!maybe_start_value.has_value()) {
        ErrorReporter::the().report(InvalidValueError {
            .value_type = "<urange>"_fly_string,
            .value_string = MUST(String::from_utf8(text)),
            .description = "Start value did not parse as hex number."_string,
        });
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
        ErrorReporter::the().report(InvalidValueError {
            .value_type = "<urange>"_fly_string,
            .value_string = MUST(String::from_utf8(text)),
            .description = "Start and end values not separated by '-'."_string,
        });
        return {};
    }

    // 6. Consume as many hex digits as possible from text.
    auto end_hex_digits = lexer.consume_while(is_ascii_hex_digit);

    //   If zero hex digits were consumed, or more than 6 hex digits were consumed,
    //   this is an invalid <urange>, and this algorithm must exit.
    if (end_hex_digits.length() == 0 || end_hex_digits.length() > 6) {
        ErrorReporter::the().report(InvalidValueError {
            .value_type = "<urange>"_fly_string,
            .value_string = MUST(String::from_utf8(text)),
            .description = MUST(String::formatted("End value had {} digits, expected between 1 and 6.", end_hex_digits.length())),
        });
        return {};
    }

    //   If there are any code points left in text, this is an invalid <urange>, and this algorithm must exit.
    if (lexer.tell_remaining() != 0) {
        ErrorReporter::the().report(InvalidValueError {
            .value_type = "<urange>"_fly_string,
            .value_string = MUST(String::from_utf8(text)),
            .description = MUST(String::formatted("Has {} trailing unused code points.", lexer.tell_remaining())),
        });
        return {};
    }

    // 7. Interpret the consumed code points as a hexadecimal number. This is the end value.
    auto maybe_end_value = AK::parse_hexadecimal_number<u32>(end_hex_digits);
    if (!maybe_end_value.has_value()) {
        ErrorReporter::the().report(InvalidValueError {
            .value_type = "<urange>"_fly_string,
            .value_string = MUST(String::from_utf8(text)),
            .description = "End value did not parse as hex number."_string,
        });
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
        if (!maybe_unicode_range.has_value())
            return {};
        unicode_ranges.append(maybe_unicode_range.release_value());
    }
    return unicode_ranges;
}

RefPtr<UnicodeRangeStyleValue const> Parser::parse_unicode_range_value(TokenStream<ComponentValue>& tokens)
{
    if (auto range = parse_unicode_range(tokens); range.has_value())
        return UnicodeRangeStyleValue::create(range.release_value());
    return nullptr;
}

RefPtr<StyleValue const> Parser::parse_integer_value(TokenStream<ComponentValue>& tokens)
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

RefPtr<StyleValue const> Parser::parse_number_value(TokenStream<ComponentValue>& tokens)
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

RefPtr<StyleValue const> Parser::parse_number_percentage_value(TokenStream<ComponentValue>& tokens)
{
    // Parses [<percentage> | <number>] (which is equivalent to [<alpha-value>])
    if (auto value = parse_number_value(tokens))
        return value;
    if (auto value = parse_percentage_value(tokens))
        return value;
    return nullptr;
}

RefPtr<StyleValue const> Parser::parse_number_percentage_none_value(TokenStream<ComponentValue>& tokens)
{
    // Parses [<percentage> | <number> | none] (which is equivalent to [<alpha-value> | none])
    if (auto value = parse_number_value(tokens))
        return value;
    if (auto value = parse_percentage_value(tokens))
        return value;

    if (tokens.next_token().is_ident("none"sv)) {
        tokens.discard_a_token(); // keyword none
        return KeywordStyleValue::create(Keyword::None);
    }

    return nullptr;
}

RefPtr<StyleValue const> Parser::parse_percentage_value(TokenStream<ComponentValue>& tokens)
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

// https://drafts.csswg.org/css-anchor-position-1/#funcdef-anchor
RefPtr<StyleValue const> Parser::parse_anchor(TokenStream<ComponentValue>& tokens)
{
    // <anchor()> = anchor( <anchor-name>? && <anchor-side>, <length-percentage>? )

    auto transaction = tokens.begin_transaction();
    auto const& function_token = tokens.consume_a_token();
    if (!function_token.is_function("anchor"sv))
        return {};

    auto argument_tokens = TokenStream { function_token.function().value };
    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.function().name });
    Optional<FlyString> anchor_name;
    RefPtr<StyleValue const> anchor_side_value;
    RefPtr<StyleValue const> fallback_value;
    for (auto i = 0; i < 2; ++i) {
        argument_tokens.discard_whitespace();

        // <anchor-name> = <dashed-ident>
        if (auto dashed_ident = parse_dashed_ident(argument_tokens); dashed_ident.has_value()) {
            if (anchor_name.has_value())
                return {};

            anchor_name = dashed_ident.value();
            continue;
        }

        if (anchor_side_value)
            break;

        // <anchor-side> = inside | outside
        //               | top | left | right | bottom
        //               | start | end | self-start | self-end
        //               | <percentage> | center
        anchor_side_value = parse_keyword_value(argument_tokens);
        if (!anchor_side_value) {
            // FIXME: Only percentages are allowed here, but we parse a length-percentage so that calc values are handled.
            anchor_side_value = parse_length_percentage_value(argument_tokens);
            if (!anchor_side_value)
                return {};

            if (anchor_side_value->is_length())
                return {};

        } else if (auto anchor_side_keyword = keyword_to_anchor_side(anchor_side_value->to_keyword()); !anchor_side_keyword.has_value()) {
            return {};
        }
    }
    if (argument_tokens.next_token().is(Token::Type::Comma)) {
        argument_tokens.discard_a_token();
        argument_tokens.discard_whitespace();
        fallback_value = parse_length_percentage_value(argument_tokens);
        if (!fallback_value) {
            fallback_value = parse_anchor(argument_tokens);
            if (!fallback_value)
                return {};
            argument_tokens.discard_a_token();
        }
    }

    if (argument_tokens.has_next_token())
        return {};

    if (!anchor_side_value)
        return {};

    return AnchorStyleValue::create(anchor_name, anchor_side_value.release_nonnull(), fallback_value);
}

// https://drafts.csswg.org/css-anchor-position-1/#sizing
RefPtr<StyleValue const> Parser::parse_anchor_size(TokenStream<ComponentValue>& tokens)
{
    // anchor-size() = anchor-size( [ <anchor-name> || <anchor-size> ]? , <length-percentage>? )

    auto transaction = tokens.begin_transaction();
    auto const& function_token = tokens.consume_a_token();
    if (!function_token.is_function("anchor-size"sv))
        return {};

    // It is only allowed in the accepted @position-try properties (and is otherwise invalid).
    static Array allowed_property_ids = {
        // inset properties
        PropertyID::Inset,
        PropertyID::Top, PropertyID::Right, PropertyID::Bottom, PropertyID::Left,
        PropertyID::InsetBlock, PropertyID::InsetBlockStart, PropertyID::InsetBlockEnd,
        PropertyID::InsetInline, PropertyID::InsetInlineStart, PropertyID::InsetInlineEnd,
        // margin properties
        PropertyID::Margin,
        PropertyID::MarginTop, PropertyID::MarginRight, PropertyID::MarginBottom, PropertyID::MarginLeft,
        PropertyID::MarginBlock, PropertyID::MarginBlockStart, PropertyID::MarginBlockEnd,
        PropertyID::MarginInline, PropertyID::MarginInlineStart, PropertyID::MarginInlineEnd,
        // sizing properties
        PropertyID::Width, PropertyID::MinWidth, PropertyID::MaxWidth,
        PropertyID::Height, PropertyID::MinHeight, PropertyID::MaxHeight,
        PropertyID::BlockSize, PropertyID::MinBlockSize, PropertyID::MaxBlockSize,
        PropertyID::InlineSize, PropertyID::MinInlineSize, PropertyID::MaxInlineSize,
        // self-alignment properties
        PropertyID::AlignSelf, PropertyID::JustifySelf, PropertyID::PlaceSelf,
        // FIXME: position-anchor
        // FIXME: position-area
    };
    bool valid_property_context = false;
    for (auto& value_context : m_value_context) {
        if (!value_context.has<PropertyID>())
            continue;
        if (!allowed_property_ids.contains_slow(value_context.get<PropertyID>())) {
            valid_property_context = false;
            break;
        }
        valid_property_context = true;
    }
    if (!valid_property_context)
        return {};

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.function().name });
    auto argument_tokens = TokenStream { function_token.function().value };

    Optional<FlyString> anchor_name;
    Optional<AnchorSize> anchor_size;
    ValueComparingRefPtr<StyleValue const> fallback_value;

    // Parse optional anchor name and anchor size in arbitrary order.
    for (auto i = 0; i < 2; ++i) {
        argument_tokens.discard_whitespace();
        auto const& peek_token = argument_tokens.next_token();
        if (!peek_token.is(Token::Type::Ident))
            break;

        // <anchor-name> = <dashed-ident>
        if (auto dashed_ident = parse_dashed_ident(argument_tokens); dashed_ident.has_value()) {
            if (anchor_name.has_value())
                return {};
            anchor_name = dashed_ident.value();
            continue;
        }

        // <anchor-size> = width | height | block | inline | self-block | self-inline
        auto keyword = keyword_from_string(peek_token.token().ident());
        if (!keyword.has_value())
            return {};
        auto maybe_anchor_size = keyword_to_anchor_size(keyword.value());
        if (!maybe_anchor_size.has_value() || anchor_size.has_value())
            return {};
        argument_tokens.discard_a_token();
        anchor_size = maybe_anchor_size.release_value();
    }

    argument_tokens.discard_whitespace();
    auto has_name_or_size = anchor_name.has_value() || anchor_size.has_value();
    auto comma_present = false;
    if (argument_tokens.next_token().is(Token::Type::Comma)) {
        if (!has_name_or_size)
            return {};
        comma_present = true;
        argument_tokens.discard_a_token();
        argument_tokens.discard_whitespace();
    }

    // FIXME: Nested anchor sizes should actually be handled by parse_length_percentage()
    if (auto nested_anchor_size = parse_anchor_size(argument_tokens))
        fallback_value = nested_anchor_size.release_nonnull();
    else if (auto length_percentage = parse_length_percentage_value(argument_tokens))
        fallback_value = length_percentage.release_nonnull();

    if (!fallback_value && comma_present)
        return {};
    if (fallback_value && !comma_present && has_name_or_size)
        return {};
    if (argument_tokens.has_next_token())
        return {};

    transaction.commit();
    return AnchorSizeStyleValue::create(anchor_name, anchor_size, fallback_value);
}

RefPtr<StyleValue const> Parser::parse_angle_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto angle_type = string_to_angle_unit(dimension_token.dimension_unit()); angle_type.has_value()) {
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

RefPtr<StyleValue const> Parser::parse_angle_percentage_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto angle_type = string_to_angle_unit(dimension_token.dimension_unit()); angle_type.has_value()) {
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

RefPtr<StyleValue const> Parser::parse_flex_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto flex_type = string_to_flex_unit(dimension_token.dimension_unit()); flex_type.has_value()) {
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

RefPtr<StyleValue const> Parser::parse_frequency_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto frequency_type = string_to_frequency_unit(dimension_token.dimension_unit()); frequency_type.has_value()) {
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

RefPtr<StyleValue const> Parser::parse_frequency_percentage_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto frequency_type = string_to_frequency_unit(dimension_token.dimension_unit()); frequency_type.has_value()) {
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

RefPtr<StyleValue const> Parser::parse_length_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto length_type = string_to_length_unit(dimension_token.dimension_unit()); length_type.has_value()) {
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

    if (tokens.next_token().is_function("anchor-size"sv))
        return parse_anchor_size(tokens);

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_length() || (calc->is_calculated() && calc->as_calculated().resolves_to_length()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<StyleValue const> Parser::parse_length_percentage_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto length_type = string_to_length_unit(dimension_token.dimension_unit()); length_type.has_value()) {
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

    if (tokens.next_token().is_function("anchor-size"sv))
        return parse_anchor_size(tokens);

    auto transaction = tokens.begin_transaction();
    if (auto calc = parse_calculated_value(tokens.consume_a_token()); calc
        && (calc->is_length() || calc->is_percentage() || (calc->is_calculated() && calc->as_calculated().resolves_to_length_percentage()))) {

        transaction.commit();
        return calc;
    }
    return nullptr;
}

RefPtr<StyleValue const> Parser::parse_resolution_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        // The allowed range of <resolution> values always excludes negative values, in addition to any explicit
        // ranges that might be specified.
        // https://drafts.csswg.org/css-values-4/#resolution
        if (dimension_token.dimension_value() < 0)
            return nullptr;
        if (auto resolution_type = string_to_resolution_unit(dimension_token.dimension_unit()); resolution_type.has_value()) {
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

RefPtr<StyleValue const> Parser::parse_time_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto time_type = string_to_time_unit(dimension_token.dimension_unit()); time_type.has_value()) {
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

RefPtr<StyleValue const> Parser::parse_time_percentage_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is(Token::Type::Dimension)) {
        auto transaction = tokens.begin_transaction();
        auto& dimension_token = tokens.consume_a_token().token();
        if (auto time_type = string_to_time_unit(dimension_token.dimension_unit()); time_type.has_value()) {
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

RefPtr<StyleValue const> Parser::parse_keyword_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Ident)) {
        auto keyword = keyword_from_string(peek_token.token().ident());
        if (keyword.has_value()) {
            tokens.discard_a_token(); // ident
            return KeywordStyleValue::create(keyword.value());
        }
    }

    return nullptr;
}

// https://www.w3.org/TR/CSS2/visufx.html#value-def-shape
RefPtr<StyleValue const> Parser::parse_rect_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto const& function_token = tokens.consume_a_token();
    if (!function_token.is_function("rect"sv))
        return nullptr;

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { "rect"sv });

    Vector<LengthOrAuto, 4> params;
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
            params.append(LengthOrAuto::make_auto());
        } else {
            auto maybe_length = parse_length(argument_tokens);
            if (!maybe_length.has_value())
                return nullptr;
            if (maybe_length.value().is_calculated()) {
                dbgln("FIXME: Support calculated lengths in rect(): {}", maybe_length.value().calculated()->to_string(CSS::SerializationMode::Normal));
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
RefPtr<StyleValue const> Parser::parse_hue_none_value(TokenStream<ComponentValue>& tokens)
{
    // Parses [<hue> | none]
    //   <hue> = <number> | <angle>

    if (auto angle = parse_angle_value(tokens))
        return angle;
    if (auto number = parse_number_value(tokens))
        return number;
    if (tokens.next_token().is_ident("none"sv)) {
        tokens.discard_a_token(); // keyword none
        return KeywordStyleValue::create(Keyword::None);
    }

    return nullptr;
}

// https://www.w3.org/TR/css-color-4/#typedef-color-alpha-value
RefPtr<StyleValue const> Parser::parse_solidus_and_alpha_value(TokenStream<ComponentValue>& tokens)
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
RefPtr<StyleValue const> Parser::parse_rgb_color_value(TokenStream<ComponentValue>& outer_tokens)
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

    RefPtr<StyleValue const> red;
    RefPtr<StyleValue const> green;
    RefPtr<StyleValue const> blue;
    RefPtr<StyleValue const> alpha;

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
        auto is_percentage = [](StyleValue const& style_value) {
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
    return RGBColorStyleValue::create(red.release_nonnull(), green.release_nonnull(), blue.release_nonnull(), alpha.release_nonnull(), legacy_syntax ? ColorSyntax::Legacy : ColorSyntax::Modern);
}

// https://www.w3.org/TR/css-color-4/#funcdef-hsl
RefPtr<StyleValue const> Parser::parse_hsl_color_value(TokenStream<ComponentValue>& outer_tokens)
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

    RefPtr<StyleValue const> h;
    RefPtr<StyleValue const> s;
    RefPtr<StyleValue const> l;
    RefPtr<StyleValue const> alpha;

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
    return HSLColorStyleValue::create(h.release_nonnull(), s.release_nonnull(), l.release_nonnull(), alpha.release_nonnull(), legacy_syntax ? ColorSyntax::Legacy : ColorSyntax::Modern);
}

// https://www.w3.org/TR/css-color-4/#funcdef-hwb
RefPtr<StyleValue const> Parser::parse_hwb_color_value(TokenStream<ComponentValue>& outer_tokens)
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

    RefPtr<StyleValue const> h;
    RefPtr<StyleValue const> w;
    RefPtr<StyleValue const> b;
    RefPtr<StyleValue const> alpha;

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
    return HWBColorStyleValue::create(h.release_nonnull(), w.release_nonnull(), b.release_nonnull(), alpha.release_nonnull());
}

Optional<Array<RefPtr<StyleValue const>, 4>> Parser::parse_lab_like_color_value(TokenStream<ComponentValue>& outer_tokens, StringView function_name)
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

    RefPtr<StyleValue const> l;
    RefPtr<StyleValue const> a;
    RefPtr<StyleValue const> b;
    RefPtr<StyleValue const> alpha;

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
RefPtr<StyleValue const> Parser::parse_lab_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // lab() = lab( [<percentage> | <number> | none]
    //      [ <percentage> | <number> | none]
    //      [ <percentage> | <number> | none]
    //      [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lab_like_color_value(outer_tokens, "lab"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return LabLikeColorStyleValue::create<LabColorStyleValue>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-oklab
RefPtr<StyleValue const> Parser::parse_oklab_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // oklab() = oklab( [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lab_like_color_value(outer_tokens, "oklab"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return LabLikeColorStyleValue::create<OKLabColorStyleValue>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

Optional<Array<RefPtr<StyleValue const>, 4>> Parser::parse_lch_like_color_value(TokenStream<ComponentValue>& outer_tokens, StringView function_name)
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

    RefPtr<StyleValue const> alpha;
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
RefPtr<StyleValue const> Parser::parse_lch_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // lch() = lch( [<percentage> | <number> | none]
    //      [ <percentage> | <number> | none]
    //      [ <hue> | none]
    //      [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lch_like_color_value(outer_tokens, "lch"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return LCHLikeColorStyleValue::create<LCHColorStyleValue>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-oklch
RefPtr<StyleValue const> Parser::parse_oklch_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // oklch() = oklch( [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <hue> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lch_like_color_value(outer_tokens, "oklch"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return LCHLikeColorStyleValue::create<OKLCHColorStyleValue>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-color
RefPtr<StyleValue const> Parser::parse_color_function(TokenStream<ComponentValue>& outer_tokens)
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
    if (!any_of(ColorFunctionStyleValue::s_supported_color_space, [&](auto supported) { return maybe_color_space.is_ident(supported); }))
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

    RefPtr<StyleValue const> alpha;
    if (inner_tokens.has_next_token()) {
        alpha = parse_solidus_and_alpha_value(inner_tokens);
        if (!alpha || inner_tokens.has_next_token())
            return {};
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();
    return ColorFunctionStyleValue::create(color_space.to_ascii_lowercase(),
        c1.release_nonnull(),
        c2.release_nonnull(),
        c3.release_nonnull(),
        alpha.release_nonnull());
}

// https://drafts.csswg.org/css-color-5/#color-mix
RefPtr<StyleValue const> Parser::parse_color_mix_function(TokenStream<ComponentValue>& tokens)
{
    auto parse_color_interpolation_method = [this](TokenStream<ComponentValue>& function_tokens) -> Optional<ColorMixStyleValue::ColorInterpolationMethod> {
        // <rectangular-color-space> = srgb | srgb-linear | display-p3 | a98-rgb | prophoto-rgb | rec2020 | lab | oklab | <xyz-space>
        // <polar-color-space> = hsl | hwb | lch | oklch
        // <custom-color-space> = <dashed-ident>
        // <hue-interpolation-method> = [ shorter | longer | increasing | decreasing ] hue
        // <color-interpolation-method> = in [ <rectangular-color-space> | <polar-color-space> <hue-interpolation-method>? | <custom-color-space> ]
        function_tokens.discard_whitespace();
        if (!function_tokens.consume_a_token().is_ident("in"sv))
            return {};
        function_tokens.discard_whitespace();

        String color_space;
        Optional<HueInterpolationMethod> hue_interpolation_method;
        auto color_space_value = parse_keyword_value(function_tokens);
        if (color_space_value) {
            auto color_space_keyword = color_space_value->to_keyword();
            color_space = MUST(String::from_utf8(string_from_keyword(color_space_keyword)));
            if (auto polar_color_space = keyword_to_polar_color_space(color_space_keyword); polar_color_space.has_value()) {
                function_tokens.discard_whitespace();
                if (auto hue_interpolation_method_keyword = parse_keyword_value(function_tokens)) {
                    hue_interpolation_method = keyword_to_hue_interpolation_method(hue_interpolation_method_keyword->to_keyword());
                    if (!hue_interpolation_method.has_value())
                        return {};

                    function_tokens.discard_whitespace();
                    if (!function_tokens.consume_a_token().is_ident("hue"sv))
                        return {};

                    function_tokens.discard_whitespace();
                }
            }
        } else {
            auto color_space_token = function_tokens.consume_a_token();
            if (color_space_token.token().type() != Token::Type::Ident)
                return {};
            color_space = color_space_token.token().ident().to_string();
        }

        function_tokens.discard_whitespace();

        auto canonical_color_space_name = [](String const& color_space_name) {
            if (color_space_name == "xyz"sv)
                return "xyz-d65"_string;
            return color_space_name;
        };

        return ColorMixStyleValue::ColorInterpolationMethod {
            .color_space = canonical_color_space_name(color_space),
            .hue_interpolation_method = hue_interpolation_method,
        };
    };

    auto parse_component = [this](TokenStream<ComponentValue>& function_tokens) -> Optional<ColorMixStyleValue::ColorMixComponent> {
        function_tokens.discard_whitespace();
        auto percentage_style_value = parse_percentage_value(function_tokens);
        function_tokens.discard_whitespace();
        auto color_style_value = parse_color_value(function_tokens);
        if (!color_style_value)
            return {};
        function_tokens.discard_whitespace();
        if (!percentage_style_value) {
            percentage_style_value = parse_percentage_value(function_tokens);
            function_tokens.discard_whitespace();
        }
        if (percentage_style_value && !percentage_style_value->is_calculated()) {
            auto percentage = percentage_style_value->as_percentage().percentage().value();
            if (percentage < 0 || percentage > 100)
                return {};
        }
        Optional<PercentageOrCalculated> percentage_or_calculated;
        if (percentage_style_value) {
            if (percentage_style_value->is_calculated()) {
                percentage_or_calculated = PercentageOrCalculated { percentage_style_value->as_calculated() };
            } else if (percentage_style_value->is_percentage()) {
                percentage_or_calculated = PercentageOrCalculated { percentage_style_value->as_percentage().percentage() };
            } else {
                VERIFY_NOT_REACHED();
            }
        }

        return ColorMixStyleValue::ColorMixComponent {
            .color = color_style_value.release_nonnull(),
            .percentage = move(percentage_or_calculated),
        };
    };

    // color-mix() = color-mix( <color-interpolation-method> , [ <color> && <percentage [0,100]>? ]#)
    // FIXME: Update color-mix to accept 1+ colors instead of exactly 2.
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    auto const& function_token = tokens.consume_a_token();
    if (!function_token.is_function("color-mix"sv))
        return {};

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.function().name });
    auto function_tokens = TokenStream { function_token.function().value };
    auto color_interpolation_method = parse_color_interpolation_method(function_tokens);
    if (!color_interpolation_method.has_value())
        return {};
    function_tokens.discard_whitespace();
    if (!function_tokens.consume_a_token().is(Token::Type::Comma))
        return {};

    auto first_component = parse_component(function_tokens);
    if (!first_component.has_value())
        return {};
    tokens.discard_whitespace();
    if (!function_tokens.consume_a_token().is(Token::Type::Comma))
        return {};

    auto second_component = parse_component(function_tokens);
    if (!second_component.has_value())
        return {};

    if (first_component->percentage.has_value() && second_component->percentage.has_value()
        && !first_component->percentage->is_calculated() && !second_component->percentage->is_calculated()
        && first_component->percentage->value().value() == 0 && second_component->percentage->value().value() == 0) {
        return {};
    }

    tokens.discard_whitespace();
    if (function_tokens.has_next_token())
        return {};

    transaction.commit();
    return ColorMixStyleValue::create(move(*color_interpolation_method), move(*first_component), move(*second_component));
}

// https://drafts.csswg.org/css-color-5/#funcdef-light-dark
RefPtr<StyleValue const> Parser::parse_light_dark_color_value(TokenStream<ComponentValue>& outer_tokens)
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
    return LightDarkStyleValue::create(light.release_nonnull(), dark.release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#color-syntax
RefPtr<StyleValue const> Parser::parse_color_value(TokenStream<ComponentValue>& tokens)
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

    if (auto color = parse_color_mix_function(tokens))
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
            return ColorStyleValue::create_from_color(color.release_value(), ColorSyntax::Legacy, ident);
        }
        // Otherwise, fall through to the hashless-hex-color case
    }

    if (component_value.is(Token::Type::Hash)) {
        auto color = Color::from_string(MUST(String::formatted("#{}", component_value.token().hash_value())));
        if (color.has_value()) {
            transaction.commit();
            return ColorStyleValue::create_from_color(color.release_value(), ColorSyntax::Legacy);
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
                [](auto const&) { return false; });
        }
        for (auto i = 1u; i < m_value_context.size() && quirky_color_allowed; i++) {
            quirky_color_allowed = m_value_context[i].visit(
                [](PropertyID const& property_id) { return property_has_quirk(property_id, Quirk::HashlessHexColor); },
                [](auto const&) { return false; });
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
                return ColorStyleValue::create_from_color(color.release_value(), ColorSyntax::Legacy);
            }
        }
    }

    return {};
}

// https://drafts.csswg.org/css-lists-3/#counter-functions
RefPtr<StyleValue const> Parser::parse_counter_value(TokenStream<ComponentValue>& tokens)
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

    auto parse_counter_style = [this](TokenStream<ComponentValue>& tokens) -> RefPtr<StyleValue const> {
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

        RefPtr<StyleValue const> counter_style;
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

        RefPtr<StyleValue const> counter_style;
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

RefPtr<StyleValue const> Parser::parse_ratio_value(TokenStream<ComponentValue>& tokens)
{
    if (auto ratio = parse_ratio(tokens); ratio.has_value())
        return RatioStyleValue::create(ratio.release_value());
    return nullptr;
}

RefPtr<StringStyleValue const> Parser::parse_string_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek = tokens.next_token();
    if (peek.is(Token::Type::String)) {
        tokens.discard_a_token();
        return StringStyleValue::create(peek.token().string());
    }

    return nullptr;
}

RefPtr<AbstractImageStyleValue const> Parser::parse_image_value(TokenStream<ComponentValue>& tokens)
{
    tokens.mark();
    auto url = parse_url_function(tokens);
    if (url.has_value()) {
        // If the value is a 'url(..)' parse as image, but if it is just a reference 'url(#xx)', leave it alone,
        // so we can parse as URL further on. These URLs are used as references inside SVG documents for masks.
        // FIXME: Remove this special case once mask-image accepts `<image>`.
        if (!url->url().starts_with('#')) {
            tokens.discard_a_mark();
            return ImageStyleValue::create(url.release_value());
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
RefPtr<StyleValue const> Parser::parse_paint_value(TokenStream<ComponentValue>& tokens)
{
    // `<paint> = none | <color> | <url> [none | <color>]? | context-fill | context-stroke`

    auto parse_color_or_none = [&]() -> Optional<RefPtr<StyleValue const>> {
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
                    return KeywordStyleValue::create(*maybe_keyword);
                default:
                    return nullptr;
                }
            }
        }

        return OptionalNone {};
    };

    // FIXME: Allow context-fill/context-stroke here
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
RefPtr<PositionStyleValue const> Parser::parse_position_value(TokenStream<ComponentValue>& tokens, PositionParsingMode position_parsing_mode)
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
    auto alternative_1 = [&]() -> RefPtr<PositionStyleValue const> {
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
    auto alternative_2 = [&]() -> RefPtr<PositionStyleValue const> {
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
    auto alternative_3 = [&]() -> RefPtr<PositionStyleValue const> {
        auto transaction = tokens.begin_transaction();

        auto parse_position_or_length = [&](bool as_horizontal) -> RefPtr<EdgeStyleValue const> {
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
    auto alternative_4 = [&]() -> RefPtr<PositionStyleValue const> {
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
    auto alternative_5_for_background_position = [&]() -> RefPtr<PositionStyleValue const> {
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

        auto to_style_value = [&](PositionAndMaybeLength const& group) -> NonnullRefPtr<EdgeStyleValue const> {
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

RefPtr<StyleValue const> Parser::parse_easing_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    tokens.discard_whitespace();

    auto const& part = tokens.consume_a_token();

    if (part.is(Token::Type::Ident)) {
        auto name = part.token().ident();
        auto maybe_simple_easing = [&] -> RefPtr<EasingStyleValue const> {
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
        }

        auto parse_argument = [this, &comma_separated_arguments](auto index) {
            TokenStream<ComponentValue> argument_tokens { comma_separated_arguments[index] };
            return parse_number(argument_tokens);
        };

        auto x1 = parse_argument(0);
        auto y1 = parse_argument(1);
        auto x2 = parse_argument(2);
        auto y2 = parse_argument(3);
        if (!x1.has_value() || !y1.has_value() || !x2.has_value() || !y2.has_value())
            return nullptr;
        if (!x1->is_calculated() && (x1->value() < 0.0 || x1->value() > 1.0))
            return nullptr;
        if (!x2->is_calculated() && (x2->value() < 0.0 || x2->value() > 1.0))
            return nullptr;

        EasingStyleValue::CubicBezier bezier {
            x1.release_value(),
            y1.release_value(),
            x2.release_value(),
            y2.release_value(),
        };

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
        auto intervals_token = TokenStream<ComponentValue>::of_single_token(intervals_argument);
        auto intervals = parse_integer(intervals_token);
        if (!intervals.has_value())
            return nullptr;

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
        if (!intervals->is_calculated()) {
            if (steps.position == EasingStyleValue::Steps::Position::JumpNone) {
                if (intervals->value() <= 1)
                    return nullptr;
            } else if (intervals->value() <= 0) {
                return nullptr;
            }
        }

        steps.number_of_intervals = *intervals;
        transaction.commit();
        return EasingStyleValue::create(steps);
    }

    return nullptr;
}

// https://drafts.csswg.org/css-values-4/#url-value
Optional<URL> Parser::parse_url_function(TokenStream<ComponentValue>& tokens)
{
    // <url> = <url()> | <src()>
    // <url()> = url( <string> <url-modifier>* ) | <url-token>
    // <src()> = src( <string> <url-modifier>* )
    auto transaction = tokens.begin_transaction();
    auto const& component_value = tokens.consume_a_token();

    // <url-token>
    if (component_value.is(Token::Type::Url)) {
        transaction.commit();
        return URL { component_value.token().url().to_string() };
    }

    // <url()> = url( <string> <url-modifier>* )
    // <src()> = src( <string> <url-modifier>* )
    if (component_value.is_function()) {
        URL::Type function_type;
        if (component_value.is_function("url"sv)) {
            function_type = URL::Type::Url;
        } else if (component_value.is_function("src"sv)) {
            function_type = URL::Type::Src;
        } else {
            return {};
        }

        auto const& function_values = component_value.function().value;
        TokenStream url_tokens { function_values };

        url_tokens.discard_whitespace();
        auto url_string = url_tokens.consume_a_token();
        if (!url_string.is(Token::Type::String))
            return {};
        url_tokens.discard_whitespace();

        // NB: Currently <request-url-modifier> is the only kind of <url-modifier>
        // https://drafts.csswg.org/css-values-5/#request-url-modifiers
        // <request-url-modifier> = <cross-origin-modifier> | <integrity-modifier> | <referrer-policy-modifier>
        Vector<RequestURLModifier> request_url_modifiers;
        // AD-HOC: This isn't mentioned in the spec, but WPT expects modifiers to be unique (one per type).
        // Spec issue: https://github.com/w3c/csswg-drafts/issues/12151
        while (url_tokens.has_next_token()) {
            auto& modifier_token = url_tokens.consume_a_token();
            if (modifier_token.is_function("cross-origin"sv)) {
                // Reject duplicates
                if (request_url_modifiers.first_matching([](auto& modifier) { return modifier.type() == RequestURLModifier::Type::CrossOrigin; }).has_value())
                    return {};
                // <cross-origin-modifier> = cross-origin(anonymous | use-credentials)
                TokenStream modifier_tokens { modifier_token.function().value };
                modifier_tokens.discard_whitespace();
                if (!modifier_tokens.next_token().is(Token::Type::Ident))
                    return {};
                auto maybe_keyword = keyword_from_string(modifier_tokens.consume_a_token().token().ident());
                modifier_tokens.discard_whitespace();
                if (!maybe_keyword.has_value() || modifier_tokens.has_next_token())
                    return {};
                if (auto value = keyword_to_cross_origin_modifier_value(*maybe_keyword); value.has_value()) {
                    request_url_modifiers.append(RequestURLModifier::create_cross_origin(value.release_value()));
                } else {
                    return {};
                }
            } else if (modifier_token.is_function("integrity"sv)) {
                // Reject duplicates
                if (request_url_modifiers.first_matching([](auto& modifier) { return modifier.type() == RequestURLModifier::Type::Integrity; }).has_value())
                    return {};
                // <integrity-modifier> = integrity(<string>)
                TokenStream modifier_tokens { modifier_token.function().value };
                modifier_tokens.discard_whitespace();
                auto& maybe_string = modifier_tokens.consume_a_token();
                modifier_tokens.discard_whitespace();
                if (!maybe_string.is(Token::Type::String) || modifier_tokens.has_next_token())
                    return {};
                request_url_modifiers.append(RequestURLModifier::create_integrity(maybe_string.token().string()));
            } else if (modifier_token.is_function("referrer-policy"sv)) {
                // Reject duplicates
                if (request_url_modifiers.first_matching([](auto& modifier) { return modifier.type() == RequestURLModifier::Type::ReferrerPolicy; }).has_value())
                    return {};

                // <referrer-policy-modifier> = (no-referrer | no-referrer-when-downgrade | same-origin | origin | strict-origin | origin-when-cross-origin | strict-origin-when-cross-origin | unsafe-url)
                TokenStream modifier_tokens { modifier_token.function().value };
                modifier_tokens.discard_whitespace();
                if (!modifier_tokens.next_token().is(Token::Type::Ident))
                    return {};
                auto maybe_keyword = keyword_from_string(modifier_tokens.consume_a_token().token().ident());
                modifier_tokens.discard_whitespace();
                if (!maybe_keyword.has_value() || modifier_tokens.has_next_token())
                    return {};
                if (auto value = keyword_to_referrer_policy_modifier_value(*maybe_keyword); value.has_value()) {
                    request_url_modifiers.append(RequestURLModifier::create_referrer_policy(value.release_value()));
                } else {
                    return {};
                }
            } else {
                ErrorReporter::the().report(InvalidValueError {
                    .value_type = "<url>"_fly_string,
                    .value_string = component_value.function().to_string(),
                    .description = MUST(String::formatted("Unrecognized URL modifier: {}", modifier_token.to_string())),
                });
                return {};
            }
            url_tokens.discard_whitespace();
        }

        // AD-HOC: This isn't mentioned in the spec, but WPT expects modifiers to be sorted alphabetically.
        // Spec issue: https://github.com/w3c/csswg-drafts/issues/12151
        quick_sort(request_url_modifiers, [](RequestURLModifier const& a, RequestURLModifier const& b) {
            return to_underlying(a.type()) < to_underlying(b.type());
        });

        transaction.commit();
        return URL { url_string.token().string().to_string(), function_type, move(request_url_modifiers) };
    }

    return {};
}

RefPtr<URLStyleValue const> Parser::parse_url_value(TokenStream<ComponentValue>& tokens)
{
    auto url = parse_url_function(tokens);
    if (!url.has_value())
        return nullptr;
    return URLStyleValue::create(url.release_value());
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

RefPtr<FitContentStyleValue const> Parser::parse_fit_content_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();

    if (component_value.is_ident("fit-content"sv)) {
        transaction.commit();
        return FitContentStyleValue::create();
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

RefPtr<StyleValue const> Parser::parse_basic_shape_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();
    if (!component_value.is_function())
        return nullptr;

    auto function_name = component_value.function().name.bytes_as_string_view();

    auto parse_fill_rule_argument = [](Vector<ComponentValue> const& component_values) -> Optional<Gfx::WindingRule> {
        TokenStream tokens { component_values };

        tokens.discard_whitespace();
        auto& maybe_ident = tokens.consume_a_token();
        tokens.discard_whitespace();

        if (tokens.has_next_token())
            return {};

        if (maybe_ident.is_ident("nonzero"sv))
            return Gfx::WindingRule::Nonzero;

        if (maybe_ident.is_ident("evenodd"sv))
            return Gfx::WindingRule::EvenOdd;

        return {};
    };

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

        auto parse_length_percentage_or_auto = [this](TokenStream<ComponentValue>& tokens) -> Optional<LengthPercentageOrAuto> {
            tokens.discard_whitespace();
            if (auto value = parse_length_percentage(tokens); value.has_value())
                return value.release_value();
            if (tokens.consume_a_token().is_ident("auto"sv))
                return LengthPercentageOrAuto::make_auto();
            return {};
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
        fill_rule = parse_fill_rule_argument(arguments[0]);

        if (fill_rule.has_value()) {
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

    if (function_name.equals_ignoring_ascii_case("path"sv)) {
        // <path()> = path( <'fill-rule'>?, <string> )
        auto arguments_tokens = TokenStream { component_value.function().value };
        auto arguments = parse_a_comma_separated_list_of_component_values(arguments_tokens);

        if (arguments.size() < 1 || arguments.size() > 2)
            return nullptr;

        // <'fill-rule'>?
        Gfx::WindingRule fill_rule { Gfx::WindingRule::Nonzero };
        if (arguments.size() == 2) {
            auto maybe_fill_rule = parse_fill_rule_argument(arguments[0]);
            if (!maybe_fill_rule.has_value())
                return nullptr;
            fill_rule = maybe_fill_rule.release_value();
        }

        // <string>, which is a path string
        TokenStream path_argument_tokens { arguments.last() };
        path_argument_tokens.discard_whitespace();
        auto& maybe_string = path_argument_tokens.consume_a_token();
        path_argument_tokens.discard_whitespace();

        if (!maybe_string.is(Token::Type::String) || path_argument_tokens.has_next_token())
            return nullptr;
        auto path_data = SVG::AttributeParser::parse_path_data(maybe_string.token().string().to_string());
        if (path_data.instructions().is_empty())
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Path { fill_rule, move(path_data) });
    }

    return nullptr;
}

RefPtr<StyleValue const> Parser::parse_builtin_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();
    if (component_value.is(Token::Type::Ident)) {
        auto ident = component_value.token().ident();
        if (ident.equals_ignoring_ascii_case("inherit"sv)) {
            transaction.commit();
            return KeywordStyleValue::create(Keyword::Inherit);
        }
        if (ident.equals_ignoring_ascii_case("initial"sv)) {
            transaction.commit();
            return KeywordStyleValue::create(Keyword::Initial);
        }
        if (ident.equals_ignoring_ascii_case("unset"sv)) {
            transaction.commit();
            return KeywordStyleValue::create(Keyword::Unset);
        }
        if (ident.equals_ignoring_ascii_case("revert"sv)) {
            transaction.commit();
            return KeywordStyleValue::create(Keyword::Revert);
        }
        if (ident.equals_ignoring_ascii_case("revert-layer"sv)) {
            transaction.commit();
            return KeywordStyleValue::create(Keyword::RevertLayer);
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

RefPtr<CustomIdentStyleValue const> Parser::parse_custom_ident_value(TokenStream<ComponentValue>& tokens, ReadonlySpan<StringView> blacklist)
{
    if (auto custom_ident = parse_custom_ident(tokens, blacklist); custom_ident.has_value())
        return CustomIdentStyleValue::create(custom_ident.release_value());
    return nullptr;
}

// https://drafts.csswg.org/css-values-4/#typedef-dashed-ident
Optional<FlyString> Parser::parse_dashed_ident(TokenStream<ComponentValue>& tokens)
{
    // The <dashed-ident> production is a <custom-ident>, with all the case-sensitivity that implies, with the
    // additional restriction that it must start with two dashes (U+002D HYPHEN-MINUS).
    auto transaction = tokens.begin_transaction();
    auto custom_ident = parse_custom_ident(tokens, {});
    if (!custom_ident.has_value() || !custom_ident->starts_with_bytes("--"sv))
        return {};
    transaction.commit();
    return custom_ident;
}

// https://www.w3.org/TR/css-grid-2/#typedef-track-breadth
Optional<GridSize> Parser::parse_grid_track_breadth(TokenStream<ComponentValue>& tokens)
{
    // <track-breadth> = <length-percentage [0,∞]> | <flex [0,∞]> | min-content | max-content | auto

    if (auto inflexible_breadth = parse_grid_inflexible_breadth(tokens); inflexible_breadth.has_value())
        return inflexible_breadth;

    // FIXME: Handle calculated flex values.
    if (auto flex_value = parse_flex_value(tokens); flex_value && flex_value->is_flex()) {
        if (auto flex = flex_value->as_flex().flex(); flex.raw_value() >= 0)
            return GridSize(flex);
    }

    return {};
}

// https://www.w3.org/TR/css-grid-2/#typedef-inflexible-breadth
Optional<GridSize> Parser::parse_grid_inflexible_breadth(TokenStream<ComponentValue>& tokens)
{
    // <inflexible-breadth>  = <length-percentage [0,∞]> | min-content | max-content | auto

    if (auto fixed_breadth = parse_grid_fixed_breadth(tokens); fixed_breadth.has_value())
        return GridSize { Size::make_length_percentage(fixed_breadth.value()) };

    auto transaction = tokens.begin_transaction();
    if (!tokens.has_next_token())
        return {};

    auto const& token = tokens.consume_a_token();
    if (token.is_ident("max-content"sv)) {
        transaction.commit();
        return GridSize(Size::make_max_content());
    }
    if (token.is_ident("min-content"sv)) {
        transaction.commit();
        return GridSize(Size::make_min_content());
    }
    if (token.is_ident("auto"sv)) {
        transaction.commit();
        return GridSize::make_auto();
    }

    return {};
}

// https://www.w3.org/TR/css-grid-2/#typedef-fixed-breadth
Optional<LengthPercentage> Parser::parse_grid_fixed_breadth(TokenStream<ComponentValue>& tokens)
{
    // <fixed-breadth> = <length-percentage [0,∞]>

    auto transaction = tokens.begin_transaction();
    auto length_percentage = parse_length_percentage(tokens);
    if (!length_percentage.has_value())
        return {};
    if (length_percentage->is_length() && length_percentage->length().raw_value() < 0)
        return {};
    if (length_percentage->is_percentage() && length_percentage->percentage().value() < 0)
        return {};
    transaction.commit();
    return length_percentage.release_value();
}

// https://www.w3.org/TR/css-grid-2/#typedef-line-names
Optional<GridLineNames> Parser::parse_grid_line_names(TokenStream<ComponentValue>& tokens)
{
    // <line-names> = '[' <custom-ident>* ']'

    auto transactions = tokens.begin_transaction();
    GridLineNames line_names;
    auto const& token = tokens.consume_a_token();
    if (!token.is_block() || !token.block().is_square())
        return line_names;

    TokenStream block_tokens { token.block().value };
    block_tokens.discard_whitespace();
    while (block_tokens.has_next_token()) {
        auto maybe_ident = parse_custom_ident(block_tokens, { { "span"sv, "auto"sv } });
        if (!maybe_ident.has_value())
            return OptionalNone {};
        line_names.append(maybe_ident.release_value());
        block_tokens.discard_whitespace();
    }

    transactions.commit();
    return line_names;
}

size_t Parser::parse_track_list_impl(TokenStream<ComponentValue>& tokens, GridTrackSizeList& output, GridTrackParser const& track_parsing_callback, AllowTrailingLineNamesForEachTrack allow_trailing_line_names_for_each_track)
{
    size_t parsed_tracks_count = 0;
    while (tokens.has_next_token()) {
        auto transaction = tokens.begin_transaction();
        auto line_names = parse_grid_line_names(tokens);

        tokens.discard_whitespace();
        auto explicit_grid_track = track_parsing_callback(tokens);
        tokens.discard_whitespace();

        if (!explicit_grid_track.has_value())
            break;

        if (line_names.has_value() && !line_names->is_empty())
            output.append(line_names.release_value());

        output.append(explicit_grid_track.release_value());
        if (allow_trailing_line_names_for_each_track == AllowTrailingLineNamesForEachTrack::Yes) {
            auto trailing_line_names = parse_grid_line_names(tokens);
            if (trailing_line_names.has_value() && !trailing_line_names->is_empty()) {
                output.append(trailing_line_names.release_value());
            }
        }
        transaction.commit();
        parsed_tracks_count++;
    }

    if (allow_trailing_line_names_for_each_track == AllowTrailingLineNamesForEachTrack::No) {
        if (auto trailing_line_names = parse_grid_line_names(tokens); trailing_line_names.has_value() && !trailing_line_names->is_empty()) {
            output.append(trailing_line_names.release_value());
        }
    }

    return parsed_tracks_count;
}

Optional<GridRepeat> Parser::parse_grid_track_repeat_impl(TokenStream<ComponentValue>& tokens, GridRepeatTypeParser const& repeat_type_parser, GridTrackParser const& repeat_track_parser)
{
    auto transaction = tokens.begin_transaction();

    if (!tokens.has_next_token())
        return {};

    auto const& token = tokens.consume_a_token();
    if (!token.is_function())
        return {};

    auto const& function_token = token.function();
    if (!function_token.name.equals_ignoring_ascii_case("repeat"sv))
        return {};
    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.name });

    auto function_tokens = TokenStream(function_token.value);
    auto comma_separated_list = parse_a_comma_separated_list_of_component_values(function_tokens);
    if (comma_separated_list.size() != 2)
        return {};

    TokenStream first_arg_tokens { comma_separated_list[0] };
    first_arg_tokens.discard_whitespace();
    if (!first_arg_tokens.has_next_token())
        return {};

    auto repeat_params = repeat_type_parser(first_arg_tokens);
    if (!repeat_params.has_value())
        return {};
    first_arg_tokens.discard_whitespace();
    if (first_arg_tokens.has_next_token())
        return {};

    TokenStream second_arg_tokens { comma_separated_list[1] };
    second_arg_tokens.discard_whitespace();
    GridTrackSizeList track_list;
    if (auto parsed_track_count = parse_track_list_impl(second_arg_tokens, track_list, repeat_track_parser); parsed_track_count == 0)
        return {};
    if (second_arg_tokens.has_next_token())
        return {};
    transaction.commit();
    return GridRepeat(GridTrackSizeList(move(track_list)), repeat_params.release_value());
}

Optional<ExplicitGridTrack> Parser::parse_grid_minmax(TokenStream<ComponentValue>& tokens, GridMinMaxParamParser const& min_parser, GridMinMaxParamParser const& max_parser)
{
    auto transaction = tokens.begin_transaction();

    if (!tokens.has_next_token())
        return {};

    auto const& token = tokens.consume_a_token();
    if (!token.is_function())
        return {};

    auto const& function_token = token.function();
    if (!function_token.name.equals_ignoring_ascii_case("minmax"sv))
        return {};

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.name });
    auto function_tokens = TokenStream(function_token.value);
    auto comma_separated_list = parse_a_comma_separated_list_of_component_values(function_tokens);
    if (comma_separated_list.size() != 2)
        return {};

    TokenStream min_tokens { comma_separated_list[0] };
    min_tokens.discard_whitespace();
    auto min_value = min_parser(min_tokens);
    if (!min_value.has_value())
        return {};
    min_tokens.discard_whitespace();
    if (min_tokens.has_next_token())
        return {};

    TokenStream max_tokens { comma_separated_list[1] };
    max_tokens.discard_whitespace();
    auto max_value = max_parser(max_tokens);
    if (!max_value.has_value())
        return {};
    max_tokens.discard_whitespace();
    if (max_tokens.has_next_token())
        return {};

    transaction.commit();
    return ExplicitGridTrack(GridMinMax(min_value.release_value(), max_value.release_value()));
}

// https://www.w3.org/TR/css-grid-2/#typedef-track-repeat
Optional<GridRepeat> Parser::parse_grid_track_repeat(TokenStream<ComponentValue>& tokens)
{
    // <track-repeat> = repeat( [ <integer [1,∞]> ] , [ <line-names>? <track-size> ]+ <line-names>? )

    GridRepeatTypeParser parse_repeat_type = [this](TokenStream<ComponentValue>& tokens) -> Optional<GridRepeatParams> {
        auto maybe_integer = parse_integer(tokens);
        if (!maybe_integer.has_value())
            return {};
        if (maybe_integer->is_calculated()) {
            // FIXME: Support calculated repeat counts.
            return {};
        }
        if (maybe_integer->value() < 1)
            return {};
        return GridRepeatParams { GridRepeatType::Fixed, static_cast<size_t>(maybe_integer->value()) };
    };
    GridTrackParser parse_track = [this](TokenStream<ComponentValue>& tokens) {
        return parse_grid_track_size(tokens);
    };
    return parse_grid_track_repeat_impl(tokens, parse_repeat_type, parse_track);
}

// https://www.w3.org/TR/css-grid-2/#typedef-auto-repeat
Optional<GridRepeat> Parser::parse_grid_auto_repeat(TokenStream<ComponentValue>& tokens)
{
    // <auto-repeat> = repeat( [ auto-fill | auto-fit ] , [ <line-names>? <fixed-size> ]+ <line-names>? )

    GridRepeatTypeParser parse_repeat_type = [](TokenStream<ComponentValue>& tokens) -> Optional<GridRepeatParams> {
        auto const& first_token = tokens.consume_a_token();
        if (!first_token.is_token() || !first_token.token().is(Token::Type::Ident))
            return {};

        auto ident_value = first_token.token().ident();
        if (ident_value.equals_ignoring_ascii_case("auto-fill"sv))
            return GridRepeatParams { GridRepeatType::AutoFill };
        if (ident_value.equals_ignoring_ascii_case("auto-fit"sv))
            return GridRepeatParams { GridRepeatType::AutoFit };
        return {};
    };
    GridTrackParser parse_track = [this](TokenStream<ComponentValue>& tokens) {
        return parse_grid_fixed_size(tokens);
    };
    return parse_grid_track_repeat_impl(tokens, parse_repeat_type, parse_track);
}

// https://www.w3.org/TR/css-grid-2/#typedef-fixed-repeat
Optional<GridRepeat> Parser::parse_grid_fixed_repeat(TokenStream<ComponentValue>& tokens)
{
    // <fixed-repeat> = repeat( [ <integer [1,∞]> ] , [ <line-names>? <fixed-size> ]+ <line-names>? )

    GridRepeatTypeParser parse_repeat_type = [this](TokenStream<ComponentValue>& tokens) -> Optional<GridRepeatParams> {
        auto maybe_integer = parse_integer(tokens);
        if (!maybe_integer.has_value())
            return {};
        if (maybe_integer->is_calculated()) {
            // FIXME: Support calculated repeat counts.
            return {};
        }
        if (maybe_integer->value() < 1)
            return {};
        return GridRepeatParams { GridRepeatType::Fixed, static_cast<size_t>(maybe_integer->value()) };
    };
    GridTrackParser parse_track = [this](TokenStream<ComponentValue>& tokens) {
        return parse_grid_fixed_size(tokens);
    };
    return parse_grid_track_repeat_impl(tokens, parse_repeat_type, parse_track);
}

// https://www.w3.org/TR/css-grid-2/#typedef-track-size
Optional<ExplicitGridTrack> Parser::parse_grid_track_size(TokenStream<ComponentValue>& tokens)
{
    // <track-size> = <track-breadth> | minmax( <inflexible-breadth> , <track-breadth> ) | fit-content( <length-percentage [0,∞]> )

    if (!tokens.has_next_token())
        return {};

    if (tokens.peek_token().is_function()) {
        auto const& token = tokens.peek_token();
        auto const& function_token = token.function();

        if (function_token.name.equals_ignoring_ascii_case("minmax"sv)) {
            GridMinMaxParamParser parse_min = [this](auto& tokens) { return parse_grid_inflexible_breadth(tokens); };
            GridMinMaxParamParser parse_max = [this](auto& tokens) { return parse_grid_track_breadth(tokens); };
            return parse_grid_minmax(tokens, parse_min, parse_max);
        }

        auto transaction = tokens.begin_transaction();
        tokens.discard_a_token();
        auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_token.name });

        if (function_token.name.equals_ignoring_ascii_case("fit-content"sv)) {
            auto function_tokens = TokenStream(function_token.value);
            function_tokens.discard_whitespace();
            auto maybe_length_percentage = parse_grid_fixed_breadth(function_tokens);
            if (!maybe_length_percentage.has_value())
                return {};
            if (function_tokens.has_next_token())
                return {};
            transaction.commit();
            return ExplicitGridTrack(GridSize(Size::make_fit_content(maybe_length_percentage.release_value())));
        }
    }

    if (auto track_breadth = parse_grid_track_breadth(tokens); track_breadth.has_value()) {
        return ExplicitGridTrack(track_breadth.value());
    }

    return {};
}

// https://www.w3.org/TR/css-grid-2/#typedef-fixed-size
Optional<ExplicitGridTrack> Parser::parse_grid_fixed_size(TokenStream<ComponentValue>& tokens)
{
    // <fixed-size> = <fixed-breadth> | minmax( <fixed-breadth> , <track-breadth> ) | minmax( <inflexible-breadth> , <fixed-breadth> )

    if (!tokens.has_next_token())
        return {};

    if (tokens.peek_token().is_function()) {
        auto const& token = tokens.peek_token();
        auto const& function_token = token.function();
        if (function_token.name.equals_ignoring_ascii_case("minmax"sv)) {
            {
                GridMinMaxParamParser parse_min = [this](auto& tokens) { return parse_grid_fixed_breadth(tokens).map([](auto& it) { return GridSize(Size::make_length_percentage(it)); }); };
                GridMinMaxParamParser parse_max = [this](auto& tokens) { return parse_grid_track_breadth(tokens); };
                if (auto result = parse_grid_minmax(tokens, parse_min, parse_max); result.has_value())
                    return result;
            }
            {
                GridMinMaxParamParser parse_min = [this](auto& tokens) { return parse_grid_inflexible_breadth(tokens); };
                GridMinMaxParamParser parse_max = [this](auto& tokens) { return parse_grid_fixed_breadth(tokens).map([](auto& it) { return GridSize(Size::make_length_percentage(it)); }); };
                if (auto result = parse_grid_minmax(tokens, parse_min, parse_max); result.has_value())
                    return result;
            }

            return {};
        }
    }

    if (auto fixed_breadth = parse_grid_fixed_breadth(tokens); fixed_breadth.has_value()) {
        return ExplicitGridTrack(GridSize { Size::make_length_percentage(fixed_breadth.release_value()) });
    }

    return {};
}

// https://www.w3.org/TR/css-grid-2/#typedef-track-list
GridTrackSizeList Parser::parse_grid_track_list(TokenStream<ComponentValue>& tokens)
{
    // <track-list> = [ <line-names>? [ <track-size> | <track-repeat> ] ]+ <line-names>?

    auto transaction = tokens.begin_transaction();
    GridTrackSizeList track_list;
    auto parsed_track_count = parse_track_list_impl(tokens, track_list, [&](auto& tokens) -> Optional<ExplicitGridTrack> {
        if (auto track_repeat = parse_grid_track_repeat(tokens); track_repeat.has_value())
            return ExplicitGridTrack(track_repeat.value());
        if (auto track_size = parse_grid_track_size(tokens); track_size.has_value())
            return ExplicitGridTrack(track_size.value());
        return Optional<ExplicitGridTrack> {};
    });
    if (parsed_track_count == 0)
        return {};
    transaction.commit();
    return track_list;
}

// https://www.w3.org/TR/css-grid-2/#typedef-auto-track-list
GridTrackSizeList Parser::parse_grid_auto_track_list(TokenStream<ComponentValue>& tokens)
{
    // <auto-track-list> = [ <line-names>? [ <fixed-size> | <fixed-repeat> ] ]* <line-names>? <auto-repeat>
    //                     [ <line-names>? [ <fixed-size> | <fixed-repeat> ] ]* <line-names>?

    auto transaction = tokens.begin_transaction();
    GridTrackSizeList track_list;
    size_t parsed_track_count = 0;
    auto parse_zero_or_more_fixed_tracks = [&] {
        parsed_track_count += parse_track_list_impl(tokens, track_list, [&](auto& tokens) -> Optional<ExplicitGridTrack> {
            if (auto fixed_repeat = parse_grid_fixed_repeat(tokens); fixed_repeat.has_value())
                return ExplicitGridTrack(fixed_repeat.value());
            if (auto fixed_size = parse_grid_fixed_size(tokens); fixed_size.has_value())
                return ExplicitGridTrack(fixed_size.value());
            return Optional<ExplicitGridTrack> {};
        });
    };

    parse_zero_or_more_fixed_tracks();
    if (!tokens.has_next_token()) {
        if (parsed_track_count == 0)
            return {};
        transaction.commit();
        return track_list;
    }

    if (auto auto_repeat = parse_grid_auto_repeat(tokens); auto_repeat.has_value()) {
        track_list.append(ExplicitGridTrack(auto_repeat.release_value()));
    } else {
        return {};
    }

    parse_zero_or_more_fixed_tracks();
    transaction.commit();
    return track_list;
}

// https://www.w3.org/TR/css-grid-2/#typedef-explicit-track-list
GridTrackSizeList Parser::parse_explicit_track_list(TokenStream<ComponentValue>& tokens)
{
    // <explicit-track-list> = [ <line-names>? <track-size> ]+ <line-names>?

    auto transaction = tokens.begin_transaction();
    GridTrackSizeList track_list;
    auto parsed_track_count = parse_track_list_impl(tokens, track_list, [&](auto& tokens) -> Optional<ExplicitGridTrack> {
        return parse_grid_track_size(tokens);
    });
    if (parsed_track_count == 0)
        return {};
    transaction.commit();
    return track_list;
}

RefPtr<GridTrackPlacementStyleValue const> Parser::parse_grid_track_placement(TokenStream<ComponentValue>& tokens)
{
    // https://www.w3.org/TR/css-grid-2/#line-placement
    // Line-based Placement: the grid-row-start, grid-column-start, grid-row-end, and grid-column-end properties
    // <grid-line> =
    //     auto |
    //     <custom-ident> |
    //     [ [ <integer [-∞,-1]> | <integer [1,∞]> ] && <custom-ident>? ] |
    //     [ span && [ <integer [1,∞]> || <custom-ident> ] ]
    bool is_span = false;
    Optional<String> parsed_custom_ident;
    Optional<IntegerOrCalculated> parsed_integer;

    auto transaction = tokens.begin_transaction();

    if (tokens.remaining_token_count() == 1 && tokens.next_token().is_ident("auto"sv)) {
        tokens.discard_a_token();
        transaction.commit();
        return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_auto());
    }

    while (tokens.has_next_token()) {
        if (tokens.next_token().is_ident("span"sv)) {
            if (is_span)
                return nullptr;

            tokens.discard_a_token();

            // NOTE: "span" must not appear in between <custom-ident> and <integer>
            if (tokens.has_next_token() && (parsed_custom_ident.has_value() || parsed_integer.has_value()))
                return nullptr;

            is_span = true;
            continue;
        }

        if (auto maybe_parsed_custom_ident = parse_custom_ident(tokens, { { "auto"sv } }); maybe_parsed_custom_ident.has_value()) {
            if (parsed_custom_ident.has_value())
                return nullptr;

            parsed_custom_ident = maybe_parsed_custom_ident->to_string();
            continue;
        }

        if (auto maybe_parsed_integer = parse_integer(tokens); maybe_parsed_integer.has_value()) {
            if (parsed_integer.has_value())
                return nullptr;

            parsed_integer = maybe_parsed_integer;
            continue;
        }

        return nullptr;
    }

    transaction.commit();

    // <custom-ident>
    // [ [ <integer [-∞,-1]> | <integer [1,∞]> ] && <custom-ident>? ]
    if (!is_span && (parsed_integer.has_value() || parsed_custom_ident.has_value()) && (!parsed_integer.has_value() || parsed_integer.value().is_calculated() || parsed_integer.value().value() != 0))
        return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line(parsed_integer, parsed_custom_ident));

    // [ span && [ <integer [1,∞]> || <custom-ident> ] ]
    if (is_span && (parsed_integer.has_value() || parsed_custom_ident.has_value()) && (!parsed_integer.has_value() || parsed_integer.value().is_calculated() || parsed_integer.value().value() > 0))
        // If the <integer> is omitted, it defaults to 1.
        return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_span(parsed_integer.value_or(1), parsed_custom_ident));

    return nullptr;
}

RefPtr<StyleValue const> Parser::parse_calculated_value(ComponentValue const& component_value)
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
                    .accepted_type_ranges = property_accepted_type_ranges(property_id),
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
                    // NOTE: Resolving percentages as numbers isn't supported by the spec and we instead expect the
                    //       caller to handle the resolved value being a percentage.
                    return CalculationContext {};
                }
                if (function.name.is_one_of_ignoring_ascii_case(
                        "rgb"sv, "rgba"sv, "hsl"sv, "hsla"sv,
                        "hwb"sv, "lab"sv, "lch"sv, "oklab"sv, "oklch"sv,
                        "color"sv)) {
                    return CalculationContext {};
                }
                // FIXME: Add other functions that provide a context for resolving values
                return {};
            },
            [](DescriptorContext const&) -> Optional<CalculationContext> {
                // FIXME: If any descriptors have `<*-percentage>` or `<integer>` types, add them here.
                return CalculationContext {};
            },
            [](SpecialContext special_context) -> Optional<CalculationContext> {
                switch (special_context) {
                case SpecialContext::ShadowBlurRadius:
                    return CalculationContext { .accepted_type_ranges = { { ValueType::Length, { 0, NumericLimits<float>::max() } } } };
                case SpecialContext::TranslateZArgument:
                    // Percentages are disallowed for the Z axis
                    return CalculationContext {};
                }
                VERIFY_NOT_REACHED();
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

RefPtr<CalculationNode const> Parser::parse_a_calc_function_node(Function const& function, CalculationContext const& context)
{
    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function.name });

    if (function.name.equals_ignoring_ascii_case("calc"sv))
        return parse_a_calculation(function.value, context);

    if (auto maybe_function = parse_math_function(function, context)) {
        // NOTE: We have to simplify manually here, since parse_math_function() is a helper for calc() parsing
        //       that doesn't do it directly by itself.
        return simplify_a_calculation_tree(*maybe_function, context, CalculationResolutionContext {});
    }

    return nullptr;
}

RefPtr<CalculationNode const> Parser::convert_to_calculation_node(CalcParsing::Node const& node, CalculationContext const& context)
{
    return node.visit(
        [this, &context](NonnullOwnPtr<CalcParsing::ProductNode> const& product_node) -> RefPtr<CalculationNode const> {
            Vector<NonnullRefPtr<CalculationNode const>> children;
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
        [this, &context](NonnullOwnPtr<CalcParsing::SumNode> const& sum_node) -> RefPtr<CalculationNode const> {
            Vector<NonnullRefPtr<CalculationNode const>> children;
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
        [this, &context](NonnullOwnPtr<CalcParsing::InvertNode> const& invert_node) -> RefPtr<CalculationNode const> {
            if (auto child_as_node = convert_to_calculation_node(invert_node->child, context))
                return InvertCalculationNode::create(child_as_node.release_nonnull());
            return nullptr;
        },
        [this, &context](NonnullOwnPtr<CalcParsing::NegateNode> const& negate_node) -> RefPtr<CalculationNode const> {
            if (auto child_as_node = convert_to_calculation_node(negate_node->child, context))
                return NegateCalculationNode::create(child_as_node.release_nonnull());
            return nullptr;
        },
        [this, &context](NonnullRawPtr<ComponentValue const> const& component_value) -> RefPtr<CalculationNode const> {
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

                if (auto length_type = string_to_length_unit(unit_string); length_type.has_value())
                    return NumericCalculationNode::create(Length { numeric_value, length_type.release_value() }, context);

                if (auto angle_type = string_to_angle_unit(unit_string); angle_type.has_value())
                    return NumericCalculationNode::create(Angle { numeric_value, angle_type.release_value() }, context);

                if (auto flex_type = string_to_flex_unit(unit_string); flex_type.has_value()) {
                    // https://www.w3.org/TR/css3-grid-layout/#fr-unit
                    // NOTE: <flex> values are not <length>s (nor are they compatible with <length>s, like some <percentage> values),
                    //       so they cannot be represented in or combined with other unit types in calc() expressions.
                    // FIXME: Flex is allowed in calc(), so figure out what this spec text means and how to implement it.
                    ErrorReporter::the().report(InvalidValueError {
                        .value_type = "math-function"_fly_string,
                        .value_string = component_value->to_string(),
                        .description = "Rejecting <flex> in math function."_string,
                    });
                    return nullptr;
                }

                if (auto frequency_type = string_to_frequency_unit(unit_string); frequency_type.has_value())
                    return NumericCalculationNode::create(Frequency { numeric_value, frequency_type.release_value() }, context);

                if (auto resolution_type = string_to_resolution_unit(unit_string); resolution_type.has_value())
                    return NumericCalculationNode::create(Resolution { numeric_value, resolution_type.release_value() }, context);

                if (auto time_type = string_to_time_unit(unit_string); time_type.has_value())
                    return NumericCalculationNode::create(Time { numeric_value, time_type.release_value() }, context);

                ErrorReporter::the().report(InvalidValueError {
                    .value_type = "math-function"_fly_string,
                    .value_string = component_value->to_string(),
                    .description = "Unrecognized dimension type."_string,
                });
                return nullptr;
            }

            if (component_value->is(Token::Type::Percentage))
                return NumericCalculationNode::create(Percentage { component_value->token().percentage() }, context);

            // NOTE: If we get here, then we have a ComponentValue that didn't get replaced with something else,
            //       so the calc() is invalid.
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "math-function"_fly_string,
                .value_string = component_value->to_string(),
                .description = "Left-over ComponentValue in calculation tree."_string,
            });
            return nullptr;
        },
        [](CalcParsing::Operator const& op) -> RefPtr<CalculationNode const> {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "math-function"_fly_string,
                .value_string = String::from_code_point(op.delim),
                .description = "Left-over Operator in calculation tree."_string,
            });
            return nullptr;
        });
}

// https://drafts.csswg.org/css-values-4/#parse-a-calculation
RefPtr<CalculationNode const> Parser::parse_a_calculation(Vector<ComponentValue> const& original_values, CalculationContext const& context)
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
            auto operator_count = 0u;
            for (size_t i = 0; i < values.size();) {
                auto& value = values[i];
                if (value.has<CalcParsing::Operator>()) {
                    operator_count++;
                    values.remove(i);
                } else {
                    i++;
                }
            }
            if (values.size() == 0 || operator_count != values.size() - 1)
                return nullptr;

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
RefPtr<StringStyleValue const> Parser::parse_opentype_tag_value(TokenStream<ComponentValue>& tokens)
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

// https://drafts.csswg.org/css-fonts/#font-face-src-parsing
RefPtr<FontSourceStyleValue const> Parser::parse_font_source_value(TokenStream<ComponentValue>& tokens)
{
    // <font-src> = <url> [ format(<font-format>)]? [ tech( <font-tech>#)]? | local(<family-name>)
    auto transaction = tokens.begin_transaction();

    tokens.discard_whitespace();

    // local(<family-name>)
    if (tokens.next_token().is_function("local"sv)) {
        auto const& function = tokens.consume_a_token().function();
        TokenStream function_tokens { function.value };
        if (auto family_name = parse_family_name_value(function_tokens)) {
            transaction.commit();
            return FontSourceStyleValue::create(FontSourceStyleValue::Local { family_name.release_nonnull() }, {}, {});
        }
        return nullptr;
    }

    // <url> [ format(<font-format>)]? [ tech( <font-tech>#)]?

    // <url>
    auto url = parse_url_function(tokens);
    if (!url.has_value())
        return nullptr;

    Optional<FlyString> format;
    Vector<FontTech> tech;

    tokens.discard_whitespace();

    // [ format(<font-format>)]?
    if (tokens.next_token().is_function("format"sv)) {
        auto const& function = tokens.consume_a_token().function();
        auto context_guard = push_temporary_value_parsing_context(FunctionContext { function.name });

        TokenStream format_tokens { function.value };
        format_tokens.discard_whitespace();
        auto const& format_name_token = format_tokens.consume_a_token();
        FlyString format_name;
        if (format_name_token.is(Token::Type::Ident)) {
            format_name = format_name_token.token().ident();
        } else if (format_name_token.is(Token::Type::String)) {
            auto& name_string = format_name_token.token().string();
            // There's a fixed set of strings allowed here, which we'll assume are case-insensitive:
            // format("woff2")                 -> format(woff2)
            // format("woff")                  -> format(woff)
            // format("truetype")              -> format(truetype)
            // format("opentype")              -> format(opentype)
            // format("collection")            -> format(collection)
            // format("woff2-variations")      -> format(woff2) tech(variations)
            // format("woff-variations")       -> format(woff) tech(variations)
            // format("truetype-variations")   -> format(truetype) tech(variations)
            // format("opentype-variations")   -> format(opentype) tech(variations)
            if (name_string.equals_ignoring_ascii_case("woff2"sv)) {
                format_name = "woff2"_fly_string;
            } else if (name_string.equals_ignoring_ascii_case("woff"sv)) {
                format_name = "woff"_fly_string;
            } else if (name_string.equals_ignoring_ascii_case("truetype"sv)) {
                format_name = "truetype"_fly_string;
            } else if (name_string.equals_ignoring_ascii_case("opentype"sv)) {
                format_name = "opentype"_fly_string;
            } else if (name_string.equals_ignoring_ascii_case("collection"sv)) {
                format_name = "collection"_fly_string;
            } else if (name_string.equals_ignoring_ascii_case("woff2-variations"sv)) {
                format_name = "woff2"_fly_string;
                tech.append(FontTech::Variations);
            } else if (name_string.equals_ignoring_ascii_case("woff-variations"sv)) {
                format_name = "woff"_fly_string;
                tech.append(FontTech::Variations);
            } else if (name_string.equals_ignoring_ascii_case("truetype-variations"sv)) {
                format_name = "truetype"_fly_string;
                tech.append(FontTech::Variations);
            } else if (name_string.equals_ignoring_ascii_case("opentype-variations"sv)) {
                format_name = "opentype"_fly_string;
                tech.append(FontTech::Variations);
            } else {
                ErrorReporter::the().report(InvalidValueError {
                    .value_type = "<font-src>"_fly_string,
                    .value_string = tokens.dump_string(),
                    .description = MUST(String::formatted("format() parameter \"{}\" is not in the set of valid strings.", name_string)),
                });
                return nullptr;
            }
        } else {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<font-src>"_fly_string,
                .value_string = tokens.dump_string(),
                .description = MUST(String::formatted("format() parameter is not an ident or string; is: {}", format_name_token.to_debug_string())),
            });
            return nullptr;
        }

        if (!font_format_is_supported(format_name)) {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<font-src>"_fly_string,
                .value_string = tokens.dump_string(),
                .description = MUST(String::formatted("format({}) is not supported.", format_name)),
            });
            return nullptr;
        }

        format_tokens.discard_whitespace();
        if (format_tokens.has_next_token()) {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<font-src>"_fly_string,
                .value_string = tokens.dump_string(),
                .description = "format() has trailing tokens."_string,
            });
            return nullptr;
        }

        format = move(format_name);
    }

    tokens.discard_whitespace();

    // [ tech( <font-tech>#)]?
    if (tokens.next_token().is_function("tech"sv)) {
        auto const& function = tokens.consume_a_token().function();
        auto context_guard = push_temporary_value_parsing_context(FunctionContext { function.name });

        TokenStream function_tokens { function.value };
        auto tech_items = parse_a_comma_separated_list_of_component_values(function_tokens);
        if (tech_items.is_empty()) {
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<font-src>"_fly_string,
                .value_string = tokens.dump_string(),
                .description = "tech() has no arguments."_string,
            });
            return nullptr;
        }

        for (auto const& tech_item : tech_items) {
            TokenStream tech_tokens { tech_item };
            tech_tokens.discard_whitespace();
            auto& ident_token = tech_tokens.consume_a_token();
            if (!ident_token.is(Token::Type::Ident)) {
                ErrorReporter::the().report(InvalidValueError {
                    .value_type = "<font-src>"_fly_string,
                    .value_string = tokens.dump_string(),
                    .description = MUST(String::formatted("tech() parameters must be idents, got: {}", ident_token.to_debug_string())),
                });
                return nullptr;
            }
            tech_tokens.discard_whitespace();
            if (tech_tokens.has_next_token()) {
                ErrorReporter::the().report(InvalidValueError {
                    .value_type = "<font-src>"_fly_string,
                    .value_string = tokens.dump_string(),
                    .description = "tech() has trailing tokens."_string,
                });
                return nullptr;
            }

            auto& font_tech_name = ident_token.token().ident();
            if (auto keyword = keyword_from_string(font_tech_name); keyword.has_value()) {
                if (auto font_tech = keyword_to_font_tech(*keyword); font_tech.has_value()) {
                    if (font_tech_is_supported(*font_tech)) {
                        tech.append(font_tech.release_value());
                        continue;
                    }
                }
            }

            ErrorReporter::the().report(InvalidValueError {
                .value_type = "<font-src>"_fly_string,
                .value_string = tokens.dump_string(),
                .description = MUST(String::formatted("tech({}) is not supported.", font_tech_name)),
            });
            return nullptr;
        }
    }

    transaction.commit();
    return FontSourceStyleValue::create(url.release_value(), move(format), move(tech));
}

NonnullRefPtr<StyleValue const> Parser::resolve_unresolved_style_value(ParsingParams const& context, DOM::AbstractElement abstract_element, PropertyIDOrCustomPropertyName property, UnresolvedStyleValue const& unresolved, Optional<GuardedSubstitutionContexts&> existing_guarded_contexts)
{
    // Unresolved always contains a var() or attr(), unless it is a custom property's value, in which case we shouldn't be trying
    // to produce a different StyleValue from it.
    VERIFY(unresolved.contains_arbitrary_substitution_function());

    auto parser = Parser::create(context, ""sv);
    if (existing_guarded_contexts.has_value())
        return parser.resolve_unresolved_style_value(abstract_element, existing_guarded_contexts.value(), property, unresolved);
    GuardedSubstitutionContexts guarded_contexts;
    return parser.resolve_unresolved_style_value(abstract_element, guarded_contexts, property, unresolved);
}

// https://drafts.csswg.org/css-values-5/#property-replacement
NonnullRefPtr<StyleValue const> Parser::resolve_unresolved_style_value(DOM::AbstractElement element, GuardedSubstitutionContexts& guarded_contexts, PropertyIDOrCustomPropertyName property, UnresolvedStyleValue const& unresolved)
{
    // AD-HOC: Report that we might rely on custom properties.
    if (unresolved.includes_attr_function())
        element.element().set_style_uses_attr_css_function();
    if (unresolved.includes_var_function())
        element.element().set_style_uses_var_css_function();

    // To replace substitution functions in a property prop:
    auto const& property_name = property.visit(
        [](PropertyID const& property_id) { return string_from_property_id(property_id); },
        [](FlyString const& name) { return name; });
    auto const& property_id = property.visit(
        [](PropertyID const& property_id) { return property_id; },
        [](FlyString const&) { return PropertyID::Custom; });

    // 1. Substitute arbitrary substitution functions in prop’s value, given «"property", prop’s name» as the
    //    substitution context. Let result be the returned component value sequence.
    auto result = substitute_arbitrary_substitution_functions(element, guarded_contexts, unresolved.values(), SubstitutionContext { SubstitutionContext::DependencyType::Property, property_name.to_string() });

    // 2. If result contains the guaranteed-invalid value, prop is invalid at computed-value time; return.
    if (contains_guaranteed_invalid_value(result))
        return GuaranteedInvalidStyleValue::create();

    // 3. Parse result according to prop’s grammar. If this returns failure, prop is invalid at computed-value time; return.
    // NB: Custom properties have no grammar as such, so we skip this step for them.
    // FIXME: Parse according to @property syntax once we support that.
    if (property_id == PropertyID::Custom)
        return UnresolvedStyleValue::create(move(result));

    auto expanded_value_tokens = TokenStream { result };
    auto parsed_value = parse_css_value(property_id, expanded_value_tokens);
    if (parsed_value.is_error())
        return GuaranteedInvalidStyleValue::create();

    // 4. Otherwise, replace prop’s value with the parsed result.
    return parsed_value.release_value();
}

RefPtr<StyleValue const> Parser::parse_value(ValueType value_type, TokenStream<ComponentValue>& tokens)
{
    switch (value_type) {
    case ValueType::Anchor:
        return parse_anchor(tokens);
    case ValueType::AnchorSize:
        return parse_anchor_size(tokens);
    case ValueType::Angle:
        return parse_angle_value(tokens);
    case ValueType::BackgroundPosition:
        return parse_position_value(tokens, PositionParsingMode::BackgroundPosition);
    case ValueType::BasicShape:
        return parse_basic_shape_value(tokens);
    case ValueType::Color:
        return parse_color_value(tokens);
    case ValueType::Counter:
        return parse_counter_value(tokens);
    case ValueType::CustomIdent:
        // FIXME: Figure out how to pass the blacklist here
        return parse_custom_ident_value(tokens, {});
    case ValueType::EasingFunction:
        return parse_easing_value(tokens);
    case ValueType::FilterValueList:
        return parse_filter_value_list_value(tokens);
    case ValueType::FitContent:
        return parse_fit_content_value(tokens);
    case ValueType::Flex:
        return parse_flex_value(tokens);
    case ValueType::Frequency:
        return parse_frequency_value(tokens);
    case ValueType::Image:
        return parse_image_value(tokens);
    case ValueType::Integer:
        return parse_integer_value(tokens);
    case ValueType::Length:
        return parse_length_value(tokens);
    case ValueType::Number:
        return parse_number_value(tokens);
    case ValueType::OpentypeTag:
        return parse_opentype_tag_value(tokens);
    case ValueType::Paint:
        return parse_paint_value(tokens);
    case ValueType::Percentage:
        return parse_percentage_value(tokens);
    case ValueType::Position:
        return parse_position_value(tokens);
    case ValueType::Ratio:
        return parse_ratio_value(tokens);
    case ValueType::Rect:
        return parse_rect_value(tokens);
    case ValueType::Resolution:
        return parse_resolution_value(tokens);
    case ValueType::String:
        return parse_string_value(tokens);
    case ValueType::Time:
        return parse_time_value(tokens);
    case ValueType::Url:
        return parse_url_value(tokens);
    }
    VERIFY_NOT_REACHED();
}

}

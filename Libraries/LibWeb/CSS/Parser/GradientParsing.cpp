/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/NonnullRawPtr.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/LinearGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>

namespace Web::CSS::Parser {

template<typename TElement>
Optional<Vector<TElement>> Parser::parse_color_stop_list(TokenStream<ComponentValue>& tokens, auto parse_position)
{
    enum class ElementType {
        Garbage,
        ColorStop,
        ColorHint
    };

    auto parse_color_stop_list_element = [&](TElement& element) -> ElementType {
        tokens.discard_whitespace();
        if (!tokens.has_next_token())
            return ElementType::Garbage;

        RefPtr<CSSStyleValue> color;
        Optional<typename TElement::PositionType> position;
        Optional<typename TElement::PositionType> second_position;
        if (position = parse_position(tokens); position.has_value()) {
            // [<T-percentage> <color>] or [<T-percentage>]
            tokens.discard_whitespace();
            // <T-percentage>
            if (!tokens.has_next_token() || tokens.next_token().is(Token::Type::Comma)) {
                element.transition_hint = typename TElement::ColorHint { *position };
                return ElementType::ColorHint;
            }
            // <T-percentage> <color>
            auto maybe_color = parse_color_value(tokens);
            if (!maybe_color)
                return ElementType::Garbage;
            color = maybe_color.release_nonnull();
        } else {
            // [<color> <T-percentage>?]
            auto maybe_color = parse_color_value(tokens);
            if (!maybe_color)
                return ElementType::Garbage;
            color = maybe_color.release_nonnull();
            tokens.discard_whitespace();
            // Allow up to [<color> <T-percentage> <T-percentage>] (double-position color stops)
            // Note: Double-position color stops only appear to be valid in this order.
            for (auto stop_position : Array { &position, &second_position }) {
                if (tokens.has_next_token() && !tokens.next_token().is(Token::Type::Comma)) {
                    *stop_position = parse_position(tokens);
                    if (!stop_position->has_value())
                        return ElementType::Garbage;
                    tokens.discard_whitespace();
                }
            }
        }

        element.color_stop = typename TElement::ColorStop { color, position, second_position };
        return ElementType::ColorStop;
    };

    TElement first_element {};
    if (parse_color_stop_list_element(first_element) != ElementType::ColorStop)
        return {};

    Vector<TElement> color_stops { first_element };
    while (tokens.has_next_token()) {
        TElement list_element {};
        tokens.discard_whitespace();
        if (!tokens.consume_a_token().is(Token::Type::Comma))
            return {};
        auto element_type = parse_color_stop_list_element(list_element);
        if (element_type == ElementType::ColorHint) {
            // <color-hint>, <color-stop>
            tokens.discard_whitespace();
            if (!tokens.consume_a_token().is(Token::Type::Comma))
                return {};
            // Note: This fills in the color stop on the same list_element as the color hint (it does not overwrite it).
            if (parse_color_stop_list_element(list_element) != ElementType::ColorStop)
                return {};
        } else if (element_type == ElementType::ColorStop) {
            // <color-stop>
        } else {
            return {};
        }
        color_stops.append(list_element);
    }

    return color_stops;
}

static StringView consume_if_starts_with(StringView str, StringView start, auto found_callback)
{
    if (str.starts_with(start, CaseSensitivity::CaseInsensitive)) {
        found_callback();
        return str.substring_view(start.length());
    }
    return str;
}

Optional<Vector<LinearColorStopListElement>> Parser::parse_linear_color_stop_list(TokenStream<ComponentValue>& tokens)
{
    // <color-stop-list> =
    //   <linear-color-stop> , [ <linear-color-hint>? , <linear-color-stop> ]#
    return parse_color_stop_list<LinearColorStopListElement>(
        tokens,
        [&](auto& it) { return parse_length_percentage(it); });
}

Optional<Vector<AngularColorStopListElement>> Parser::parse_angular_color_stop_list(TokenStream<ComponentValue>& tokens)
{
    // <angular-color-stop-list> =
    //   <angular-color-stop> , [ <angular-color-hint>? , <angular-color-stop> ]#
    return parse_color_stop_list<AngularColorStopListElement>(
        tokens,
        [&](auto& it) { return parse_angle_percentage(it); });
}

RefPtr<CSSStyleValue> Parser::parse_linear_gradient_function(TokenStream<ComponentValue>& outer_tokens)
{
    using GradientType = LinearGradientStyleValue::GradientType;

    auto transaction = outer_tokens.begin_transaction();
    auto& component_value = outer_tokens.consume_a_token();

    if (!component_value.is_function())
        return nullptr;

    GradientRepeating repeating_gradient = GradientRepeating::No;
    GradientType gradient_type { GradientType::Standard };

    auto function_name = component_value.function().name.bytes_as_string_view();

    function_name = consume_if_starts_with(function_name, "-webkit-"sv, [&] {
        gradient_type = GradientType::WebKit;
    });

    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_name });

    function_name = consume_if_starts_with(function_name, "repeating-"sv, [&] {
        repeating_gradient = GradientRepeating::Yes;
    });

    if (!function_name.equals_ignoring_ascii_case("linear-gradient"sv))
        return nullptr;

    // linear-gradient() = linear-gradient([ <angle> | to <side-or-corner> ]?, <color-stop-list>)

    TokenStream tokens { component_value.function().value };
    tokens.discard_whitespace();

    if (!tokens.has_next_token())
        return nullptr;

    bool has_direction_param = true;
    LinearGradientStyleValue::GradientDirection gradient_direction = gradient_type == GradientType::Standard
        ? SideOrCorner::Bottom
        : SideOrCorner::Top;

    auto to_side = [](StringView value) -> Optional<SideOrCorner> {
        if (value.equals_ignoring_ascii_case("top"sv))
            return SideOrCorner::Top;
        if (value.equals_ignoring_ascii_case("bottom"sv))
            return SideOrCorner::Bottom;
        if (value.equals_ignoring_ascii_case("left"sv))
            return SideOrCorner::Left;
        if (value.equals_ignoring_ascii_case("right"sv))
            return SideOrCorner::Right;
        return {};
    };

    auto is_to_side_or_corner = [&](auto const& token) {
        if (!token.is(Token::Type::Ident))
            return false;
        if (gradient_type == GradientType::WebKit)
            return to_side(token.token().ident()).has_value();
        return token.token().ident().equals_ignoring_ascii_case("to"sv);
    };

    auto const& first_param = tokens.next_token();
    if (first_param.is(Token::Type::Dimension)) {
        // <angle>
        tokens.discard_a_token();
        auto angle_value = first_param.token().dimension_value();
        auto unit_string = first_param.token().dimension_unit();
        auto angle_type = Angle::unit_from_name(unit_string);

        if (!angle_type.has_value())
            return nullptr;

        gradient_direction = Angle { angle_value, angle_type.release_value() };
    } else if (is_to_side_or_corner(first_param)) {
        // <side-or-corner> = [left | right] || [top | bottom]

        // Note: -webkit-linear-gradient does not include to the "to" prefix on the side or corner
        if (gradient_type == GradientType::Standard) {
            tokens.discard_a_token();
            tokens.discard_whitespace();

            if (!tokens.has_next_token())
                return nullptr;
        }

        // [left | right] || [top | bottom]
        auto const& first_side = tokens.consume_a_token();
        if (!first_side.is(Token::Type::Ident))
            return nullptr;

        auto side_a = to_side(first_side.token().ident());
        tokens.discard_whitespace();
        Optional<SideOrCorner> side_b;
        if (tokens.has_next_token() && tokens.next_token().is(Token::Type::Ident))
            side_b = to_side(tokens.consume_a_token().token().ident());

        if (side_a.has_value() && !side_b.has_value()) {
            gradient_direction = *side_a;
        } else if (side_a.has_value() && side_b.has_value()) {
            // Convert two sides to a corner
            if (to_underlying(*side_b) < to_underlying(*side_a))
                swap(side_a, side_b);
            if (side_a == SideOrCorner::Top && side_b == SideOrCorner::Left)
                gradient_direction = SideOrCorner::TopLeft;
            else if (side_a == SideOrCorner::Top && side_b == SideOrCorner::Right)
                gradient_direction = SideOrCorner::TopRight;
            else if (side_a == SideOrCorner::Bottom && side_b == SideOrCorner::Left)
                gradient_direction = SideOrCorner::BottomLeft;
            else if (side_a == SideOrCorner::Bottom && side_b == SideOrCorner::Right)
                gradient_direction = SideOrCorner::BottomRight;
            else
                return nullptr;
        } else {
            return nullptr;
        }
    } else {
        has_direction_param = false;
    }

    tokens.discard_whitespace();
    if (!tokens.has_next_token())
        return nullptr;

    if (has_direction_param && !tokens.consume_a_token().is(Token::Type::Comma))
        return nullptr;

    auto color_stops = parse_linear_color_stop_list(tokens);
    if (!color_stops.has_value())
        return nullptr;

    transaction.commit();
    return LinearGradientStyleValue::create(gradient_direction, move(*color_stops), gradient_type, repeating_gradient);
}

RefPtr<CSSStyleValue> Parser::parse_conic_gradient_function(TokenStream<ComponentValue>& outer_tokens)
{
    auto transaction = outer_tokens.begin_transaction();
    auto& component_value = outer_tokens.consume_a_token();

    if (!component_value.is_function())
        return nullptr;

    GradientRepeating repeating_gradient = GradientRepeating::No;

    auto function_name = component_value.function().name.bytes_as_string_view();
    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_name });

    function_name = consume_if_starts_with(function_name, "repeating-"sv, [&] {
        repeating_gradient = GradientRepeating::Yes;
    });

    if (!function_name.equals_ignoring_ascii_case("conic-gradient"sv))
        return nullptr;

    TokenStream tokens { component_value.function().value };
    tokens.discard_whitespace();

    if (!tokens.has_next_token())
        return nullptr;

    Angle from_angle(0, Angle::Type::Deg);
    RefPtr<PositionStyleValue> at_position;

    // conic-gradient( [ [ from <angle> ]? [ at <position> ]? ]  ||
    // <color-interpolation-method> , <angular-color-stop-list> )
    NonnullRawPtr<ComponentValue const> token = tokens.next_token();
    bool got_from_angle = false;
    bool got_color_interpolation_method = false;
    bool got_at_position = false;
    while (token->is(Token::Type::Ident)) {
        auto consume_identifier = [&](auto identifier) {
            auto token_string = token->token().ident();
            if (token_string.equals_ignoring_ascii_case(identifier)) {
                tokens.discard_a_token();
                tokens.discard_whitespace();
                return true;
            }
            return false;
        };

        if (consume_identifier("from"sv)) {
            // from <angle>
            if (got_from_angle || got_at_position)
                return nullptr;
            if (!tokens.has_next_token())
                return nullptr;

            auto const& angle_token = tokens.consume_a_token();
            if (!angle_token.is(Token::Type::Dimension))
                return nullptr;
            auto angle = angle_token.token().dimension_value();
            auto angle_unit = angle_token.token().dimension_unit();
            auto angle_type = Angle::unit_from_name(angle_unit);
            if (!angle_type.has_value())
                return nullptr;

            from_angle = Angle(angle, *angle_type);
            got_from_angle = true;
        } else if (consume_identifier("at"sv)) {
            // at <position>
            if (got_at_position)
                return nullptr;
            auto position = parse_position_value(tokens);
            if (!position)
                return nullptr;
            at_position = position;
            got_at_position = true;
        } else if (consume_identifier("in"sv)) {
            // <color-interpolation-method>
            if (got_color_interpolation_method)
                return nullptr;
            dbgln("FIXME: Parse color interpolation method for conic-gradient()");
            got_color_interpolation_method = true;
        } else {
            break;
        }
        tokens.discard_whitespace();
        if (!tokens.has_next_token())
            return nullptr;
        token = tokens.next_token();
    }

    tokens.discard_whitespace();
    if (!tokens.has_next_token())
        return nullptr;
    if ((got_from_angle || got_at_position || got_color_interpolation_method) && !tokens.consume_a_token().is(Token::Type::Comma))
        return nullptr;

    auto color_stops = parse_angular_color_stop_list(tokens);
    if (!color_stops.has_value())
        return nullptr;

    if (!at_position)
        at_position = PositionStyleValue::create_center();

    transaction.commit();
    return ConicGradientStyleValue::create(from_angle, at_position.release_nonnull(), move(*color_stops), repeating_gradient);
}

RefPtr<CSSStyleValue> Parser::parse_radial_gradient_function(TokenStream<ComponentValue>& outer_tokens)
{
    using EndingShape = RadialGradientStyleValue::EndingShape;
    using Extent = RadialGradientStyleValue::Extent;
    using CircleSize = RadialGradientStyleValue::CircleSize;
    using EllipseSize = RadialGradientStyleValue::EllipseSize;
    using Size = RadialGradientStyleValue::Size;

    auto transaction = outer_tokens.begin_transaction();
    auto& component_value = outer_tokens.consume_a_token();

    if (!component_value.is_function())
        return nullptr;

    auto repeating_gradient = GradientRepeating::No;

    auto function_name = component_value.function().name.bytes_as_string_view();
    auto context_guard = push_temporary_value_parsing_context(FunctionContext { function_name });

    function_name = consume_if_starts_with(function_name, "repeating-"sv, [&] {
        repeating_gradient = GradientRepeating::Yes;
    });

    if (!function_name.equals_ignoring_ascii_case("radial-gradient"sv))
        return nullptr;

    TokenStream tokens { component_value.function().value };
    tokens.discard_whitespace();
    if (!tokens.has_next_token())
        return nullptr;

    bool expect_comma = false;

    auto commit_value = [&]<typename... T>(auto value, T&... transactions) {
        (transactions.commit(), ...);
        return value;
    };

    // radial-gradient( [ <ending-shape> || <size> ]? [ at <position> ]? , <color-stop-list> )

    Size size = Extent::FarthestCorner;
    EndingShape ending_shape = EndingShape::Circle;
    RefPtr<PositionStyleValue> at_position;

    auto parse_ending_shape = [&]() -> Optional<EndingShape> {
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        auto& token = tokens.consume_a_token();
        if (!token.is(Token::Type::Ident))
            return {};
        auto ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("circle"sv))
            return commit_value(EndingShape::Circle, transaction);
        if (ident.equals_ignoring_ascii_case("ellipse"sv))
            return commit_value(EndingShape::Ellipse, transaction);
        return {};
    };

    auto parse_extent_keyword = [](StringView keyword) -> Optional<Extent> {
        if (keyword.equals_ignoring_ascii_case("closest-corner"sv))
            return Extent::ClosestCorner;
        if (keyword.equals_ignoring_ascii_case("closest-side"sv))
            return Extent::ClosestSide;
        if (keyword.equals_ignoring_ascii_case("farthest-corner"sv))
            return Extent::FarthestCorner;
        if (keyword.equals_ignoring_ascii_case("farthest-side"sv))
            return Extent::FarthestSide;
        return {};
    };

    auto parse_size = [&]() -> Optional<Size> {
        // <size> =
        //      <extent-keyword>              |
        //      <length [0,∞]>                |
        //      <length-percentage [0,∞]>{2}
        auto transaction_size = tokens.begin_transaction();
        tokens.discard_whitespace();
        if (!tokens.has_next_token())
            return {};
        if (tokens.next_token().is(Token::Type::Ident)) {
            auto extent = parse_extent_keyword(tokens.consume_a_token().token().ident());
            if (!extent.has_value())
                return {};
            return commit_value(*extent, transaction_size);
        }
        auto first_radius = parse_length_percentage(tokens);
        if (!first_radius.has_value())
            return {};
        auto transaction_second_dimension = tokens.begin_transaction();
        tokens.discard_whitespace();
        if (tokens.has_next_token()) {
            auto second_radius = parse_length_percentage(tokens);
            if (second_radius.has_value())
                return commit_value(EllipseSize { first_radius.release_value(), second_radius.release_value() },
                    transaction_size, transaction_second_dimension);
        }
        // FIXME: Support calculated lengths
        if (first_radius->is_length())
            return commit_value(CircleSize { first_radius->length() }, transaction_size);
        return {};
    };

    {
        // [ <ending-shape> || <size> ]?
        auto maybe_ending_shape = parse_ending_shape();
        auto maybe_size = parse_size();
        if (!maybe_ending_shape.has_value() && maybe_size.has_value())
            maybe_ending_shape = parse_ending_shape();
        if (maybe_size.has_value()) {
            size = *maybe_size;
            expect_comma = true;
        }
        if (maybe_ending_shape.has_value()) {
            expect_comma = true;
            ending_shape = *maybe_ending_shape;
            if (ending_shape == EndingShape::Circle && size.has<EllipseSize>())
                return nullptr;
            if (ending_shape == EndingShape::Ellipse && size.has<CircleSize>())
                return nullptr;
        } else {
            ending_shape = size.has<CircleSize>() ? EndingShape::Circle : EndingShape::Ellipse;
        }
    }

    tokens.discard_whitespace();
    if (!tokens.has_next_token())
        return nullptr;

    auto& token = tokens.next_token();
    if (token.is_ident("at"sv)) {
        tokens.discard_a_token();
        auto position = parse_position_value(tokens);
        if (!position)
            return nullptr;
        at_position = position;
        expect_comma = true;
    }

    tokens.discard_whitespace();
    if (!tokens.has_next_token())
        return nullptr;
    if (expect_comma && !tokens.consume_a_token().is(Token::Type::Comma))
        return nullptr;

    // <color-stop-list>
    auto color_stops = parse_linear_color_stop_list(tokens);
    if (!color_stops.has_value())
        return nullptr;

    if (!at_position)
        at_position = PositionStyleValue::create_center();

    transaction.commit();
    return RadialGradientStyleValue::create(ending_shape, size, at_position.release_nonnull(), move(*color_stops), repeating_gradient);
}

}

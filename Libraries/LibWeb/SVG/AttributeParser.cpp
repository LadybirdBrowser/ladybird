/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/StringConversions.h>
#include <AK/Utf16View.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/PathParserRustFFI.h>

namespace Web::SVG {

static SVG::RustFFI::FfiSvgPathInput ffi_svg_path_input(Utf16View);

AttributeParser::AttributeParser(Utf16View source)
    : m_lexer(source)
{
}

Optional<Vector<Transform>> AttributeParser::parse_transform(Utf16View input)
{
    AttributeParser parser { input };
    return parser.parse_transform();
}

Path AttributeParser::parse_path_data(Utf16View input)
{
    auto* path = SVG::RustFFI::rust_parse_svg_path_data(ffi_svg_path_input(input));
    return path ? Path { path } : Path {};
}

static SVG::RustFFI::FfiSvgPathInput ffi_svg_path_input(Utf16View input)
{
    return {
        .ascii = input.has_ascii_storage() ? reinterpret_cast<u8 const*>(input.ascii_span().data()) : nullptr,
        .utf16 = input.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(input.utf16_span().data()),
        .length = input.length_in_code_units(),
    };
}

Optional<i32> AttributeParser::parse_integer(Utf16View input)
{
    AttributeParser parser { input };
    parser.parse_whitespace();
    auto result_or_error = parser.parse_integer();
    if (result_or_error.is_error())
        return {};
    parser.parse_whitespace();
    if (parser.done())
        return result_or_error.value();

    return {};
}

float NumberPercentage::resolve_relative_to(float length) const
{
    if (!m_is_percentage)
        return m_value;
    return m_value * length;
}

Optional<NumberPercentage> AttributeParser::parse_number_percentage(Utf16View input)
{
    AttributeParser parser { input };
    parser.parse_whitespace();

    auto number_or_error = parser.parse_number();
    if (number_or_error.is_error())
        return {};
    bool is_percentage = parser.match('%');
    if (is_percentage)
        parser.consume();
    parser.parse_whitespace();
    if (parser.done())
        return NumberPercentage(number_or_error.value(), is_percentage);

    return {};
}

Vector<Gfx::FloatPoint> AttributeParser::parse_points(Utf16View input)
{
    AttributeParser parser { input };

    parser.parse_whitespace();

    Vector<Gfx::FloatPoint> points;

    auto result = parser.parse_coordinate_pair_sequence([&](Gfx::FloatPoint point) {
        points.append(point);
    });

    if (result.is_error())
        return {};

    return points;
}

ErrorOr<float> AttributeParser::parse_length()
{
    // https://www.w3.org/TR/SVG11/types.html#DataTypeLength
    return parse_number();
}

ErrorOr<float> AttributeParser::parse_coordinate()
{
    // https://www.w3.org/TR/SVG11/types.html#DataTypeCoordinate
    // coordinate ::= length
    return parse_length();
}

// https://www.w3.org/TR/SVG11/types.html#DataTypeInteger
ErrorOr<i32> AttributeParser::parse_integer()
{
    if (!match_integer())
        return Error::from_string_literal("Expected integer");

    auto parse_result = AK::parse_first_number<i32>(m_lexer.remaining(), TrimWhitespace::No);
    if (!parse_result.has_value())
        return Error::from_string_literal("Integer out of range");

    m_lexer.ignore(parse_result->characters_parsed);
    return parse_result->value;
}

ErrorOr<Gfx::FloatPoint> AttributeParser::parse_coordinate_pair()
{
    auto x = TRY(parse_coordinate());
    if (match_comma_whitespace())
        parse_comma_whitespace();
    auto y = TRY(parse_coordinate());
    return Gfx::FloatPoint { x, y };
}

ErrorOr<void> AttributeParser::parse_coordinate_pair_sequence(Function<void(Gfx::FloatPoint)> const& callback)
{
    bool is_first = true;
    while (true) {
        auto coordinate_pair_or_error = parse_coordinate_pair();
        if (coordinate_pair_or_error.is_error()) {
            if (is_first)
                return Error::from_string_literal("Expected coordinate pair sequence");
            break;
        }
        is_first = false;
        callback(coordinate_pair_or_error.release_value());
        if (match_comma_whitespace())
            parse_comma_whitespace();
        if (!match_comma_whitespace() && !match_coordinate())
            break;
    }
    return {};
}

void AttributeParser::parse_whitespace(bool must_match_once)
{
    bool matched = false;
    while (!done() && match_whitespace()) {
        consume();
        matched = true;
    }

    VERIFY(!must_match_once || matched);
}

void AttributeParser::parse_comma_whitespace()
{
    if (match(',')) {
        consume();
        parse_whitespace();
    } else {
        parse_whitespace(1);
        if (match(','))
            consume();
        parse_whitespace();
    }
}

// https://www.w3.org/TR/SVG11/types.html#DataTypeNumber
ErrorOr<float> AttributeParser::parse_number()
{
    auto sign = parse_sign();
    return sign * TRY(parse_nonnegative_number());
}

// https://www.w3.org/TR/SVG11/paths.html#PathDataBNF
ErrorOr<float> AttributeParser::parse_nonnegative_number()
{
    // NOTE: The grammar is almost a floating point except we cannot have a sign
    //       at the start. That condition should have been checked by the caller.
    if (match('+') || match('-') || !match_number())
        return Error::from_string_literal("Expected number");

    auto parse_result = AK::parse_first_number<float>(m_lexer.remaining(), TrimWhitespace::No);
    VERIFY(parse_result.has_value());

    m_lexer.ignore(parse_result->characters_parsed);
    return parse_result->value;
}

int AttributeParser::parse_sign()
{
    if (match('-')) {
        consume();
        return -1;
    }
    if (match('+'))
        consume();
    return 1;
}

static bool whitespace(char16_t c)
{
    // wsp:
    // Either a U+000A LINE FEED, U+000D CARRIAGE RETURN, U+0009 CHARACTER TABULATION, or U+0020 SPACE.
    return AK::first_is_one_of(c, '\n', '\r', '\t', '\f', ' ');
}

// https://svgwg.org/svg2-draft/coords.html#PreserveAspectRatioAttribute
Optional<PreserveAspectRatio> AttributeParser::parse_preserve_aspect_ratio(Utf16View input)
{
    // <align> <meetOrSlice>?
    Utf16GenericLexer lexer { input };
    lexer.ignore_while(whitespace);
    auto align_string = lexer.consume_until(whitespace);
    if (align_string.is_empty())
        return {};
    lexer.ignore_while(whitespace);
    auto meet_or_slice_string = lexer.consume_until(whitespace);

    // <align> =
    //     none
    //     | xMinYMin | xMidYMin | xMaxYMin
    //     | xMinYMid | xMidYMid | xMaxYMid
    //     | xMinYMax | xMidYMax | xMaxYMax
    auto align = [&]() -> Optional<PreserveAspectRatio::Align> {
        if (align_string == "none"sv)
            return PreserveAspectRatio::Align::None;
        if (align_string == "xMinYMin"sv)
            return PreserveAspectRatio::Align::xMinYMin;
        if (align_string == "xMidYMin"sv)
            return PreserveAspectRatio::Align::xMidYMin;
        if (align_string == "xMaxYMin"sv)
            return PreserveAspectRatio::Align::xMaxYMin;
        if (align_string == "xMinYMid"sv)
            return PreserveAspectRatio::Align::xMinYMid;
        if (align_string == "xMidYMid"sv)
            return PreserveAspectRatio::Align::xMidYMid;
        if (align_string == "xMaxYMid"sv)
            return PreserveAspectRatio::Align::xMaxYMid;
        if (align_string == "xMinYMax"sv)
            return PreserveAspectRatio::Align::xMinYMax;
        if (align_string == "xMidYMax"sv)
            return PreserveAspectRatio::Align::xMidYMax;
        if (align_string == "xMaxYMax"sv)
            return PreserveAspectRatio::Align::xMaxYMax;
        return {};
    }();

    if (!align.has_value())
        return {};

    // <meetOrSlice> = meet | slice
    auto meet_or_slice = [&]() -> Optional<PreserveAspectRatio::MeetOrSlice> {
        if (meet_or_slice_string.is_empty() || meet_or_slice_string == "meet"sv)
            return PreserveAspectRatio::MeetOrSlice::Meet;
        if (meet_or_slice_string == "slice"sv)
            return PreserveAspectRatio::MeetOrSlice::Slice;
        return {};
    }();

    if (!meet_or_slice.has_value())
        return {};

    return PreserveAspectRatio { *align, *meet_or_slice };
}

// https://svgwg.org/svg2-draft/pservers.html#LinearGradientElementGradientUnitsAttribute
// https://drafts.fxtf.org/css-masking/#element-attrdef-mask-maskunits
// https://drafts.fxtf.org/css-masking/#element-attrdef-mask-maskcontentunits
Optional<SVGUnits> AttributeParser::parse_units(Utf16View input)
{
    Utf16GenericLexer lexer { input };
    lexer.ignore_while(whitespace);
    auto gradient_units_string = lexer.consume_until(whitespace);
    if (gradient_units_string == "userSpaceOnUse"sv)
        return SVGUnits::UserSpaceOnUse;
    if (gradient_units_string == "objectBoundingBox"sv)
        return SVGUnits::ObjectBoundingBox;
    return {};
}

// https://svgwg.org/svg2-draft/pservers.html#RadialGradientElementSpreadMethodAttribute
Optional<SpreadMethod> AttributeParser::parse_spread_method(Utf16View input)
{
    Utf16GenericLexer lexer { input };
    lexer.ignore_while(whitespace);
    auto spread_method_string = lexer.consume_until(whitespace);
    if (spread_method_string == "pad"sv)
        return SpreadMethod::Pad;
    if (spread_method_string == "repeat"sv)
        return SpreadMethod::Repeat;
    if (spread_method_string == "reflect"sv)
        return SpreadMethod::Reflect;
    return {};
}

// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-tablevalues
Vector<float> AttributeParser::parse_table_values(Utf16View input)
{
    Vector<float> table_values;

    AttributeParser parser { input };
    while (!parser.done()) {
        parser.parse_whitespace();
        auto table_value = parser.parse_nonnegative_number();
        if (table_value.is_error())
            return {};

        table_values.append(table_value.release_value());
        parser.parse_whitespace();
        if (parser.match(','))
            parser.consume();
    }

    return table_values;
}

// https://drafts.csswg.org/css-transforms/#svg-syntax
Optional<Vector<Transform>> AttributeParser::parse_transform()
{
    auto consume_whitespace = [&] {
        m_lexer.ignore_while(whitespace);
    };

    auto consume_comma_whitespace = [&] {
        consume_whitespace();
        m_lexer.consume_specific(',');
        consume_whitespace();
    };

    // FIXME: This parsing is quite lenient, so will accept (with default values) some transforms that should be rejected.
    auto parse_optional_number = [&](float default_value = 0.0f) {
        consume_comma_whitespace();
        auto number_or_error = parse_number();
        if (number_or_error.is_error())
            return default_value;
        return number_or_error.value();
    };

    auto try_parse_number = [&]() -> Optional<float> {
        auto number_or_error = parse_number();
        if (number_or_error.is_error())
            return {};
        return number_or_error.value();
    };

    auto parse_function = [&](auto body) -> Optional<Transform> {
        consume_whitespace();
        if (!m_lexer.consume_specific('('))
            return {};
        consume_whitespace();
        auto maybe_operation = body();
        if (!maybe_operation.has_value())
            return {};
        Transform transform { .operation = Transform::Operation { *maybe_operation } };
        consume_whitespace();
        if (m_lexer.consume_specific(')'))
            return transform;
        return {};
    };

    // NOTE: This looks very similar to the CSS transform but the syntax is not compatible.
    Vector<Transform> transform_list;
    consume_whitespace();
    while (!done()) {
        Optional<Transform> maybe_transform;
        if (m_lexer.consume_specific("translate"sv)) {
            maybe_transform = parse_function([&]() -> Optional<Transform::Translate> {
                Transform::Translate translate {};
                auto maybe_x = try_parse_number();
                if (!maybe_x.has_value())
                    return {};
                translate.x = *maybe_x;
                translate.y = parse_optional_number();
                return translate;
            });
        } else if (m_lexer.consume_specific("scale"sv)) {
            maybe_transform = parse_function([&]() -> Optional<Transform::Scale> {
                Transform::Scale scale {};
                auto maybe_x = try_parse_number();
                if (!maybe_x.has_value())
                    return {};
                scale.x = *maybe_x;
                scale.y = parse_optional_number(scale.x);
                return scale;
            });
        } else if (m_lexer.consume_specific("rotate"sv)) {
            maybe_transform = parse_function([&]() -> Optional<Transform::Rotate> {
                Transform::Rotate rotate {};
                auto maybe_a = try_parse_number();
                if (!maybe_a.has_value())
                    return {};
                rotate.a = *maybe_a;
                rotate.x = parse_optional_number();
                rotate.y = parse_optional_number();
                return rotate;
            });
        } else if (m_lexer.consume_specific("skewX"sv)) {
            maybe_transform = parse_function([&]() -> Optional<Transform::SkewX> {
                Transform::SkewX skew_x {};
                auto maybe_a = try_parse_number();
                if (!maybe_a.has_value())
                    return {};
                skew_x.a = *maybe_a;
                return skew_x;
            });
        } else if (m_lexer.consume_specific("skewY"sv)) {
            maybe_transform = parse_function([&]() -> Optional<Transform::SkewY> {
                Transform::SkewY skew_y {};
                auto maybe_a = try_parse_number();
                if (!maybe_a.has_value())
                    return {};
                skew_y.a = *maybe_a;
                return skew_y;
            });
        } else if (m_lexer.consume_specific("matrix"sv)) {
            maybe_transform = parse_function([&]() -> Optional<Transform::Matrix> {
                Transform::Matrix matrix;
                auto maybe_a = try_parse_number();
                if (!maybe_a.has_value())
                    return {};
                matrix.a = *maybe_a;
                consume_comma_whitespace();
                auto maybe_b = try_parse_number();
                if (!maybe_b.has_value())
                    return {};
                matrix.b = *maybe_b;
                consume_comma_whitespace();
                auto maybe_c = try_parse_number();
                if (!maybe_c.has_value())
                    return {};
                matrix.c = *maybe_c;
                consume_comma_whitespace();
                auto maybe_d = try_parse_number();
                if (!maybe_d.has_value())
                    return {};
                matrix.d = *maybe_d;
                consume_comma_whitespace();
                auto maybe_e = try_parse_number();
                if (!maybe_e.has_value())
                    return {};
                matrix.e = *maybe_e;
                consume_comma_whitespace();
                auto maybe_f = try_parse_number();
                if (!maybe_f.has_value())
                    return {};
                matrix.f = *maybe_f;
                return matrix;
            });
        }
        if (maybe_transform.has_value())
            transform_list.append(*maybe_transform);
        else
            return {};
        consume_comma_whitespace();
    }
    return transform_list;
}

Optional<ViewBox> AttributeParser::parse_viewbox(Utf16View input)
{
    AttributeParser parser { input };
    ViewBox viewbox;

    parser.parse_whitespace();
    auto maybe_min_x = parser.parse_coordinate();
    if (maybe_min_x.is_error())
        return {};
    viewbox.min_x = maybe_min_x.value();

    if (!parser.match_comma_whitespace())
        return {};
    parser.parse_comma_whitespace();
    auto maybe_min_y = parser.parse_coordinate();
    if (maybe_min_y.is_error())
        return {};
    viewbox.min_y = maybe_min_y.value();

    if (!parser.match_comma_whitespace())
        return {};
    parser.parse_comma_whitespace();
    auto maybe_width = parser.parse_length();
    if (maybe_width.is_error())
        return {};
    viewbox.width = maybe_width.value();

    if (!parser.match_comma_whitespace())
        return {};
    parser.parse_comma_whitespace();
    auto maybe_height = parser.parse_length();
    if (maybe_height.is_error())
        return {};
    viewbox.height = maybe_height.value();

    parser.parse_whitespace();
    if (!parser.done())
        return {};

    return viewbox;
}

bool AttributeParser::match_whitespace() const
{
    if (done())
        return false;
    char16_t c = ch();
    return c == 0x9 || c == 0x20 || c == 0xa || c == 0xc || c == 0xd;
}

bool AttributeParser::match_comma_whitespace() const
{
    return match_whitespace() || match(',');
}

bool AttributeParser::match_coordinate() const
{
    return match_length(AllowDot::Yes);
}

bool AttributeParser::match_number() const
{
    return match_length(AllowDot::Yes);
}

bool AttributeParser::match_integer() const
{
    return match_length(AllowDot::No);
}

bool AttributeParser::match_length(AllowDot allow_dot) const
{
    if (done())
        return false;

    size_t offset = 0;
    if (ch() == '-' || ch() == '+')
        offset++;

    if (allow_dot == AllowDot::Yes && ch(offset) == '.')
        offset++;

    return !done() && is_ascii_digit(ch(offset));
}

}

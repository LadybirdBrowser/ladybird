/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StringConversions.h>
#include <LibWeb/CSS/Parser/FastColorParsing.h>

namespace Web::CSS::Parser {

namespace {

// CSS preprocessing converts carriage returns and form feeds to line feeds before tokenization.
static constexpr auto css_whitespace_code_points = u" \n\t\f\r"sv;

struct LegacyRGBComponent {
    double value;
    bool is_percentage;
};

// https://www.w3.org/TR/css-syntax-3/#consume-number
static bool has_valid_css_number_syntax(Utf16View value)
{
    if (value.is_empty())
        return false;

    size_t offset = 0;

    if (value.code_unit_at(offset) == '+' || value.code_unit_at(offset) == '-') {
        ++offset;
        if (offset == value.length_in_code_units())
            return false;
    }

    bool has_digits = false;
    while (offset < value.length_in_code_units() && is_ascii_digit(value.code_unit_at(offset))) {
        has_digits = true;
        ++offset;
    }

    if (offset < value.length_in_code_units() && value.code_unit_at(offset) == '.') {
        ++offset;
        if (offset == value.length_in_code_units() || !is_ascii_digit(value.code_unit_at(offset)))
            return false;
        while (offset < value.length_in_code_units() && is_ascii_digit(value.code_unit_at(offset)))
            ++offset;
    } else if (!has_digits) {
        return false;
    }

    if (offset < value.length_in_code_units() && to_ascii_lowercase(value.code_unit_at(offset)) == 'e') {
        ++offset;
        if (offset < value.length_in_code_units() && (value.code_unit_at(offset) == '+' || value.code_unit_at(offset) == '-'))
            ++offset;
        if (offset == value.length_in_code_units() || !is_ascii_digit(value.code_unit_at(offset)))
            return false;
        while (offset < value.length_in_code_units() && is_ascii_digit(value.code_unit_at(offset)))
            ++offset;
    }

    return offset == value.length_in_code_units();
}

static Optional<LegacyRGBComponent> parse_legacy_rgb_component(Utf16View input)
{
    auto value = input.trim(css_whitespace_code_points);
    if (value.is_empty())
        return {};

    bool is_percentage = value.ends_with('%');
    if (is_percentage) {
        value = value.substring_view(0, value.length_in_code_units() - 1);
    }

    if (value.is_empty() || !has_valid_css_number_syntax(value))
        return {};

    auto number = AK::parse_number<double>(value, TrimWhitespace::No);
    if (!number.has_value() || !isfinite(*number))
        return {};

    return LegacyRGBComponent { *number, is_percentage };
}

static Optional<u8> parse_alpha_component(Utf16View input)
{
    auto component = parse_legacy_rgb_component(input);
    if (!component.has_value())
        return {};

    auto alpha = component->is_percentage ? component->value / 100.0 : component->value;
    return static_cast<u8>(llround(clamp(alpha * 255.0, 0.0, 255.0)));
}

// https://www.w3.org/TR/css-color-4/#funcdef-rgb
static Optional<Gfx::Color> parse_legacy_rgb_color(Utf16View input)
{
    // rgb() = [ <legacy-rgb-syntax> | <modern-rgb-syntax> ]
    // rgba() = [ <legacy-rgba-syntax> | <modern-rgba-syntax> ]
    // <legacy-rgb-syntax> = rgb( <percentage>#{3} , <alpha-value>? ) |
    //                       rgb( <number>#{3} , <alpha-value>? )
    // <legacy-rgba-syntax> = rgba( <percentage>#{3} , <alpha-value>? ) |
    //                        rgba( <number>#{3} , <alpha-value>? )

    size_t function_name_length;
    if (input.length_in_code_units() >= 4 && input.substring_view(0, 4).equals_ignoring_ascii_case(u"rgb("sv))
        function_name_length = 4;
    else if (input.length_in_code_units() >= 5 && input.substring_view(0, 5).equals_ignoring_ascii_case(u"rgba("sv))
        function_name_length = 5;
    else
        return {};

    if (!input.ends_with(')'))
        return {};

    auto components = input.substring_view(function_name_length, input.length_in_code_units() - function_name_length - 1).split_view(u',', SplitBehavior::KeepEmpty);
    if (components.size() != 3 && components.size() != 4)
        return {};

    Array<LegacyRGBComponent, 3> rgb_components;
    for (size_t index = 0; index < rgb_components.size(); ++index) {
        auto component = parse_legacy_rgb_component(components[index]);
        if (!component.has_value())
            return {};
        rgb_components[index] = *component;
    }

    if (rgb_components[0].is_percentage != rgb_components[1].is_percentage
        || rgb_components[0].is_percentage != rgb_components[2].is_percentage)
        return {};

    auto to_byte = [is_percentage = rgb_components[0].is_percentage](double value) {
        if (is_percentage)
            value = value * 255.0 / 100.0;
        return static_cast<u8>(llround(clamp(value, 0.0, 255.0)));
    };

    u8 alpha = 255;
    if (components.size() == 4) {
        auto parsed_alpha = parse_alpha_component(components[3]);
        if (!parsed_alpha.has_value())
            return {};
        alpha = *parsed_alpha;
    }

    return Gfx::Color(to_byte(rgb_components[0].value), to_byte(rgb_components[1].value), to_byte(rgb_components[2].value), alpha);
}

}

Optional<Gfx::Color> parse_simple_color(Utf16View input)
{
    auto value = input.trim(css_whitespace_code_points);
    if (value.is_empty())
        return {};

    if (value.starts_with('#'))
        return Gfx::Color::from_utf16_string(value);

    if (value.equals_ignoring_ascii_case(u"transparent"sv))
        return Gfx::Color::Transparent;

    if (auto color = Gfx::Color::from_named_css_color_string(value); color.has_value())
        return color;

    return parse_legacy_rgb_color(value);
}

}

/*
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Math.h>
#include <LibGfx/Gradients.h>
#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/LinearGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/GradientPainting.h>

namespace Web::Painting {

static ColorStopList replace_transition_hints_with_normal_color_stops(ColorStopList const& color_stop_list)
{
    ColorStopList stops_with_replaced_transition_hints;

    auto const& first_color_stop = color_stop_list.first();
    // First color stop in the list should never have transition hint value
    VERIFY(!first_color_stop.transition_hint.has_value());
    stops_with_replaced_transition_hints.empend(first_color_stop.color, first_color_stop.position);

    // This loop replaces transition hints with five regular points, calculated using the
    // formula defined in the spec. After rendering using linear interpolation, this will
    // produce a result close enough to that obtained if the color of each point were calculated
    // using the non-linear formula from the spec.
    for (size_t i = 1; i < color_stop_list.size(); i++) {
        auto const& color_stop = color_stop_list[i];
        if (!color_stop.transition_hint.has_value()) {
            stops_with_replaced_transition_hints.empend(color_stop.color, color_stop.position);
            continue;
        }

        auto const& previous_color_stop = color_stop_list[i - 1];
        auto const& next_color_stop = color_stop_list[i];

        auto distance_between_stops = next_color_stop.position - previous_color_stop.position;
        auto transition_hint = color_stop.transition_hint.value();

        Array transition_hint_relative_sampling_positions {
            transition_hint * 0.33f,
            transition_hint * 0.66f,
            transition_hint,
            transition_hint + (1.f - transition_hint) * 0.33f,
            transition_hint + (1.f - transition_hint) * 0.66f,
        };

        for (auto const& transition_hint_relative_sampling_position : transition_hint_relative_sampling_positions) {
            auto position = previous_color_stop.position + transition_hint_relative_sampling_position * distance_between_stops;
            auto value = Gfx::color_stop_step(previous_color_stop, next_color_stop, position);
            auto color = previous_color_stop.color.interpolate(next_color_stop.color, value);
            stops_with_replaced_transition_hints.empend(color, position);
        }

        stops_with_replaced_transition_hints.empend(color_stop.color, color_stop.position);
    }

    return stops_with_replaced_transition_hints;
}

static ColorStopList expand_repeat_length(ColorStopList const& color_stop_list, float repeat_length)
{
    // https://drafts.csswg.org/css-images/#repeating-gradients
    // When rendered, however, the color-stops are repeated infinitely in both directions, with their
    // positions shifted by multiples of the difference between the last specified color-stop's position
    // and the first specified color-stop's position. For example, repeating-linear-gradient(red 10px, blue 50px)
    // is equivalent to linear-gradient(..., red -30px, blue 10px, red 10px, blue 50px, red 50px, blue 90px, ...).

    auto first_stop_position = color_stop_list.first().position;
    int const negative_repeat_count = AK::ceil(first_stop_position / repeat_length);
    int const positive_repeat_count = AK::ceil((1.0f - first_stop_position) / repeat_length);

    ColorStopList color_stop_list_with_expanded_repeat = color_stop_list;

    auto get_color_between_stops = [](float position, auto const& current_stop, auto const& previous_stop) {
        auto distance_between_stops = current_stop.position - previous_stop.position;
        auto percentage = (position - previous_stop.position) / distance_between_stops;
        return previous_stop.color.interpolate(current_stop.color, percentage);
    };

    for (auto repeat_count = 1; repeat_count <= negative_repeat_count; repeat_count++) {
        for (auto stop : color_stop_list.in_reverse()) {
            stop.position -= repeat_length * static_cast<float>(repeat_count);
            if (stop.position < 0) {
                stop.color = get_color_between_stops(0.0f, stop, color_stop_list_with_expanded_repeat.first());
                stop.position = 0.0f;
                color_stop_list_with_expanded_repeat.prepend(stop);
                break;
            }
            color_stop_list_with_expanded_repeat.prepend(stop);
        }
    }

    for (auto repeat_count = 1; repeat_count < positive_repeat_count; repeat_count++) {
        for (auto stop : color_stop_list) {
            stop.position += repeat_length * static_cast<float>(repeat_count);
            if (stop.position > 1) {
                stop.color = get_color_between_stops(1.0f, stop, color_stop_list_with_expanded_repeat.last());
                stop.position = 1.0f;
                color_stop_list_with_expanded_repeat.append(stop);
                break;
            }
            color_stop_list_with_expanded_repeat.append(stop);
        }
    }

    return color_stop_list_with_expanded_repeat;
}

static ColorStopList expand_color_stops_for_painting(ColorStopList const& color_stop_list, Optional<float> repeat_length)
{
    auto expanded = repeat_length.has_value()
        ? expand_repeat_length(color_stop_list, repeat_length.value())
        : color_stop_list;
    return replace_transition_hints_with_normal_color_stops(expanded);
}

static ColorStopData resolve_color_stop_positions(Layout::NodeWithStyle const& node, Vector<CSS::ColorStopListElement> const& color_stop_list, auto resolve_position_to_float, bool repeating)
{
    VERIFY(!color_stop_list.is_empty());
    ColorStopList resolved_color_stops;

    auto color_stop_length = [&](auto& stop) {
        return stop.color_stop.second_position ? 2 : 1;
    };

    size_t expanded_size = 0;
    for (auto& stop : color_stop_list)
        expanded_size += color_stop_length(stop);

    resolved_color_stops.ensure_capacity(expanded_size);
    for (auto& stop : color_stop_list) {
        auto resolved_stop = Gfx::ColorStop { .color = stop.color_stop.color->to_color(CSS::ColorResolutionContext::for_layout_node_with_style(node)).value() };
        for (int i = 0; i < color_stop_length(stop); i++)
            resolved_color_stops.append(resolved_stop);
    }

    // 1. If the first color stop does not have a position, set its position to 0%.
    resolved_color_stops.first().position = 0;
    //    If the last color stop does not have a position, set its position to 100%
    resolved_color_stops.last().position = 1.0f;

    // 2. If a color stop or transition hint has a position that is less than the
    //    specified position of any color stop or transition hint before it in the list,
    //    set its position to be equal to the largest specified position of any color stop
    //    or transition hint before it.
    auto max_previous_color_stop_or_hint = resolved_color_stops[0].position;
    auto resolve_stop_position = [&](CSS::StyleValue const& position) {
        float value = resolve_position_to_float(position);
        value = max(value, max_previous_color_stop_or_hint);
        max_previous_color_stop_or_hint = value;
        return value;
    };
    size_t resolved_index = 0;
    for (auto& stop : color_stop_list) {
        if (stop.transition_hint)
            resolved_color_stops[resolved_index].transition_hint = resolve_stop_position(*stop.transition_hint);
        if (stop.color_stop.position)
            resolved_color_stops[resolved_index].position = resolve_stop_position(*stop.color_stop.position);
        if (stop.color_stop.second_position)
            resolved_color_stops[++resolved_index].position = resolve_stop_position(*stop.color_stop.second_position);
        ++resolved_index;
    }

    // 3. If any color stop still does not have a position, then, for each run of adjacent color stops
    //    without positions, set their positions so that they are evenly spaced between the preceding
    //    and following color stops with positions.
    // Note: Though not mentioned anywhere in the specification transition hints are counted as "color stops with positions".
    size_t i = 1;
    auto find_run_end = [&] {
        auto color_stop_has_position = [](auto& color_stop) {
            return color_stop.transition_hint.has_value() || isfinite(color_stop.position);
        };
        while (i < color_stop_list.size() - 1 && !color_stop_has_position(resolved_color_stops[i])) {
            i++;
        }
        return i;
    };
    while (i < resolved_color_stops.size() - 1) {
        auto& stop = resolved_color_stops[i];
        if (!isfinite(stop.position)) {
            auto run_start = i - 1;
            auto start_position = resolved_color_stops[i++].transition_hint.value_or(resolved_color_stops[run_start].position);
            auto run_end = find_run_end();
            auto end_position = resolved_color_stops[run_end].transition_hint.value_or(resolved_color_stops[run_end].position);
            auto spacing = (end_position - start_position) / (run_end - run_start);
            for (auto j = run_start + 1; j < run_end; j++) {
                resolved_color_stops[j].position = start_position + (j - run_start) * spacing;
            }
        }
        i++;
    }

    // Determine the location of the transition hint as a percentage of the distance between the two color stops,
    // denoted as a number between 0 and 1, where 0 indicates the hint is placed right on the first color stop,
    // and 1 indicates the hint is placed right on the second color stop.
    for (size_t i = 1; i < resolved_color_stops.size(); i++) {
        auto& color_stop = resolved_color_stops[i];
        auto& previous_color_stop = resolved_color_stops[i - 1];
        if (color_stop.transition_hint.has_value()) {
            auto stop_length = color_stop.position - previous_color_stop.position;
            color_stop.transition_hint = stop_length > 0 ? (*color_stop.transition_hint - previous_color_stop.position) / stop_length : 0;
        }
    }

    Optional<float> repeat_length = {};
    if (repeating)
        repeat_length = resolved_color_stops.last().position - resolved_color_stops.first().position;

    return { resolved_color_stops, repeat_length, repeating };
}

LinearGradientData resolve_linear_gradient_data(Layout::NodeWithStyle const& node, CSSPixelSize gradient_size, CSS::LinearGradientStyleValue const& linear_gradient)
{
    auto gradient_angle = linear_gradient.angle_degrees(gradient_size);
    auto gradient_length_px = Gfx::calculate_gradient_length(gradient_size.to_type<float>(), gradient_angle);

    CSS::CalculationResolutionContext context {
        .percentage_basis = CSS::Length::make_px(gradient_length_px),
    };
    auto resolved_color_stops = resolve_color_stop_positions(
        node, linear_gradient.color_stop_list(), [&](auto const& position) -> float {
            if (position.is_length())
                return position.as_length().length().absolute_length_to_px_without_rounding() / gradient_length_px;
            if (position.is_percentage())
                return position.as_percentage().percentage().as_fraction();
            return position.as_calculated().resolve_length(context)->absolute_length_to_px_without_rounding() / gradient_length_px;
        },
        linear_gradient.is_repeating());

    // Replace transition hints for painting; keep repeat_length for Skia's native tiling
    resolved_color_stops.list = replace_transition_hints_with_normal_color_stops(resolved_color_stops.list);

    return { gradient_angle, resolved_color_stops, linear_gradient.interpolation_method() };
}

ConicGradientData resolve_conic_gradient_data(Layout::NodeWithStyle const& node, CSS::ConicGradientStyleValue const& conic_gradient)
{
    CSS::Angle const one_turn { 360.0f, CSS::AngleUnit::Deg };
    auto resolved_color_stops = resolve_color_stop_positions(
        node, conic_gradient.color_stop_list(), [&](auto const& position) -> float {
            return CSS::Angle::from_style_value(position, one_turn).to_degrees() / one_turn.to_degrees();
        },
        conic_gradient.is_repeating());

    // Expand color stops for painting (replace transition hints and expand repeat length)
    resolved_color_stops.list = expand_color_stops_for_painting(resolved_color_stops.list, resolved_color_stops.repeat_length);
    resolved_color_stops.repeat_length = {};

    return { conic_gradient.angle_degrees(), resolved_color_stops, conic_gradient.interpolation_method() };
}

RadialGradientData resolve_radial_gradient_data(Layout::NodeWithStyle const& node, CSSPixelSize gradient_size, CSS::RadialGradientStyleValue const& radial_gradient)
{
    CSS::CalculationResolutionContext context {
        .percentage_basis = CSS::Length::make_px(gradient_size.width()),
    };

    // Start center, goes right to ending point, where the gradient line intersects the ending shape
    auto resolved_color_stops = resolve_color_stop_positions(
        node, radial_gradient.color_stop_list(), [&](auto const& position) -> float {
            if (position.is_length())
                return position.as_length().length().absolute_length_to_px_without_rounding() / gradient_size.width().to_float();
            if (position.is_percentage())
                return position.as_percentage().percentage().as_fraction();
            return position.as_calculated().resolve_length(context)->absolute_length_to_px_without_rounding() / gradient_size.width().to_float();
        },
        radial_gradient.is_repeating());

    // Expand color stops for painting (replace transition hints and expand repeat length)
    resolved_color_stops.list = expand_color_stops_for_painting(resolved_color_stops.list, resolved_color_stops.repeat_length);
    resolved_color_stops.repeat_length = {};

    return { resolved_color_stops, radial_gradient.interpolation_method() };
}

}

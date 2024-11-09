/*
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGfx/Gradients.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/LinearGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/GradientPainting.h>

namespace Web::Painting {

static ColorStopData resolve_color_stop_positions(Layout::NodeWithStyleAndBoxModelMetrics const& node, auto const& color_stop_list, auto resolve_position_to_float, bool repeating)
{
    VERIFY(color_stop_list.size() >= 2);
    ColorStopList resolved_color_stops;

    auto color_stop_length = [&](auto& stop) {
        return stop.color_stop.second_position.has_value() ? 2 : 1;
    };

    size_t expanded_size = 0;
    for (auto& stop : color_stop_list)
        expanded_size += color_stop_length(stop);

    resolved_color_stops.ensure_capacity(expanded_size);
    for (auto& stop : color_stop_list) {
        auto resolved_stop = Gfx::ColorStop { .color = stop.color_stop.color->to_color(node) };
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
    auto resolve_stop_position = [&](auto& position) {
        float value = resolve_position_to_float(position);
        value = max(value, max_previous_color_stop_or_hint);
        max_previous_color_stop_or_hint = value;
        return value;
    };
    size_t resolved_index = 0;
    for (auto& stop : color_stop_list) {
        if (stop.transition_hint.has_value())
            resolved_color_stops[resolved_index].transition_hint = resolve_stop_position(stop.transition_hint->value);
        if (stop.color_stop.position.has_value())
            resolved_color_stops[resolved_index].position = resolve_stop_position(*stop.color_stop.position);
        if (stop.color_stop.second_position.has_value())
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

    return { resolved_color_stops, repeat_length };
}

LinearGradientData resolve_linear_gradient_data(Layout::NodeWithStyleAndBoxModelMetrics const& node, CSSPixelSize gradient_size, CSS::LinearGradientStyleValue const& linear_gradient)
{
    auto gradient_angle = linear_gradient.angle_degrees(gradient_size);
    auto gradient_length_px = Gfx::calculate_gradient_length(gradient_size.to_type<float>(), gradient_angle);

    auto resolved_color_stops = resolve_color_stop_positions(
        node, linear_gradient.color_stop_list(), [&](auto const& length_percentage) {
            return length_percentage.to_px(node, CSSPixels::nearest_value_for(gradient_length_px)).to_float() / static_cast<float>(gradient_length_px);
        },
        linear_gradient.is_repeating());

    return { gradient_angle, resolved_color_stops };
}

ConicGradientData resolve_conic_gradient_data(Layout::NodeWithStyleAndBoxModelMetrics const& node, CSS::ConicGradientStyleValue const& conic_gradient)
{
    CSS::Angle one_turn(360.0f, CSS::Angle::Type::Deg);
    auto resolved_color_stops = resolve_color_stop_positions(
        node, conic_gradient.color_stop_list(), [&](auto const& angle_percentage) {
            return angle_percentage.resolved(node, one_turn).to_degrees() / one_turn.to_degrees();
        },
        conic_gradient.is_repeating());
    return { conic_gradient.angle_degrees(), resolved_color_stops };
}

RadialGradientData resolve_radial_gradient_data(Layout::NodeWithStyleAndBoxModelMetrics const& node, CSSPixelSize gradient_size, CSS::RadialGradientStyleValue const& radial_gradient)
{
    // Start center, goes right to ending point, where the gradient line intersects the ending shape
    auto resolved_color_stops = resolve_color_stop_positions(
        node, radial_gradient.color_stop_list(), [&](auto const& length_percentage) {
            return length_percentage.to_px(node, gradient_size.width()).to_float() / gradient_size.width().to_float();
        },
        radial_gradient.is_repeating());
    return { resolved_color_stops };
}

}

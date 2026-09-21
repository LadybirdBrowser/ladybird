/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

struct LinearEasingFunction {
    struct ControlPoint {
        double input;
        double output;

        bool operator==(ControlPoint const&) const = default;
    };

    Vector<ControlPoint> control_points;
    Utf16String stringified;

    double evaluate_at(double input_progress, bool before_flag) const;

    bool operator==(LinearEasingFunction const&) const = default;
};

struct CubicBezierEasingFunction {
    double x1;
    double y1;
    double x2;
    double y2;
    Utf16String stringified;

    double evaluate_at(double input_progress, bool before_flag) const;

    bool operator==(CubicBezierEasingFunction const&) const = default;
};

struct StepsEasingFunction {
    i32 interval_count;
    StepPosition position;
    Utf16String stringified;

    double evaluate_at(double input_progress, bool before_flag) const;

    bool operator==(StepsEasingFunction const&) const = default;
};

struct EasingFunction : public Variant<LinearEasingFunction, CubicBezierEasingFunction, StepsEasingFunction> {
    using Variant::Variant;

    static EasingFunction linear();
    static EasingFunction ease_in();
    static EasingFunction ease_out();
    static EasingFunction ease_in_out();
    static EasingFunction ease();

    static EasingFunction from_style_value(StyleValue const&);

    double evaluate_at(double input_progress, bool before_flag) const;
    String to_string() const;
    Utf16String const& to_utf16_string() const;
};

// The easing as one of the generated Rust FFI namespaces describes it. A linear easing's control
// points go into the storage the caller provides, which the descriptor borrows.
template<typename FfiEasingDescriptor, typename FfiLinearEasingPoint>
FfiEasingDescriptor to_ffi_easing_descriptor(EasingFunction const& easing, Vector<FfiLinearEasingPoint>& linear_points)
{
    using FfiEasingKind = decltype(FfiEasingDescriptor::kind);
    FfiEasingDescriptor descriptor {};
    easing.visit(
        [&](LinearEasingFunction const& linear) {
            descriptor.kind = FfiEasingKind::Linear;
            linear_points.ensure_capacity(linear.control_points.size());
            for (auto const& point : linear.control_points)
                linear_points.unchecked_append({ .input = point.input, .output = point.output });
            descriptor.linear_points = linear_points.data();
            descriptor.linear_point_count = linear_points.size();
        },
        [&](CubicBezierEasingFunction const& cubic_bezier) {
            descriptor.kind = FfiEasingKind::CubicBezier;
            descriptor.x1 = cubic_bezier.x1;
            descriptor.y1 = cubic_bezier.y1;
            descriptor.x2 = cubic_bezier.x2;
            descriptor.y2 = cubic_bezier.y2;
        },
        [&](StepsEasingFunction const& steps) {
            descriptor.kind = FfiEasingKind::Steps;
            descriptor.interval_count = steps.interval_count;
            descriptor.step_position = to_underlying(steps.position);
        });
    return descriptor;
}

}

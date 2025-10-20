/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

struct LinearEasingFunction {
    struct ControlPoint {
        Optional<double> input;
        double output;
    };

    Vector<ControlPoint> control_points;
    String stringified;

    double evaluate_at(double input_progress, bool before_flag) const;
};

struct CubicBezierEasingFunction {
    double x1;
    double y1;
    double x2;
    double y2;
    String stringified;

    struct CachedSample {
        double x;
        double y;
        double t;
    };

    mutable Vector<CachedSample> m_cached_x_samples {};

    double evaluate_at(double input_progress, bool before_flag) const;
};

struct StepsEasingFunction {
    i64 interval_count;
    StepPosition position;
    String stringified;

    double evaluate_at(double input_progress, bool before_flag) const;
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
};

}

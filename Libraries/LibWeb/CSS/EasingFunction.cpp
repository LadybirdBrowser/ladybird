/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-easing-2/#linear-easing-function
EasingFunction EasingFunction::linear()
{
    // Equivalent to linear(0, 1)
    EasingFunction easing;
    easing.m_descriptor.kind = Compositing::RustFFI::FfiEasingKind::Linear;
    easing.m_linear_points = { { 0, 0 }, { 1, 1 } };
    easing.m_text = "linear"_utf16;
    return easing;
}

// https://drafts.csswg.org/css-easing-2/#valdef-cubic-bezier-easing-function-ease
EasingFunction EasingFunction::ease()
{
    // Equivalent to cubic-bezier(0.25, 0.1, 0.25, 1).
    EasingFunction easing;
    easing.m_descriptor.kind = Compositing::RustFFI::FfiEasingKind::CubicBezier;
    easing.m_descriptor.x1 = 0.25;
    easing.m_descriptor.y1 = 0.1;
    easing.m_descriptor.x2 = 0.25;
    easing.m_descriptor.y2 = 1;
    easing.m_text = "ease"_utf16;
    return easing;
}

EasingFunction EasingFunction::from_style_value(StyleValue const& style_value)
{
    EasingFunction easing;
    auto append_point = [](void* points, LinearPoint point) {
        static_cast<Vector<LinearPoint>*>(points)->append(point);
    };
    VERIFY(StyleValueFFI::rust_easing_from_style_value(style_value.rust_style_value_data(), &easing.m_descriptor, &easing.m_linear_points, append_point));
    auto serialization_mode = style_value.is_easing() && easing.m_descriptor.kind == Compositing::RustFFI::FfiEasingKind::CubicBezier
        ? SerializationMode::Normal
        : SerializationMode::ResolvedValue;
    easing.m_text = style_value.to_utf16_string(serialization_mode);
    return easing;
}

double EasingFunction::evaluate_at(double input_progress, bool before_flag) const
{
    auto descriptor = this->descriptor();
    return StyleValueFFI::rust_evaluate_easing(&descriptor, input_progress, before_flag);
}

}

/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>

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

Optional<EasingFunction> EasingFunction::parse(Utf16View value)
{
    if (auto style_value = parse_css_value(Parser::ParsingParams(), value, PropertyID::AnimationTimingFunction)) {
        if (style_value->is_unresolved() || style_value->is_css_wide_keyword())
            return {};

        auto easing_values = style_value->as_value_list().values();

        if (easing_values.size() != 1)
            return {};

        // FIXME: We should absolutize the style value to resolve relative lengths within calcs
        return from_style_value(easing_values[0]);
    }

    return {};
}

}

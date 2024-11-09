/*
 * Copyright (c) 2024, Steffen T. Larssen <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibWeb/CSS/StyleValues/CSSMathValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

#include "RotationStyleValue.h"

namespace Web::CSS {

// https://www.w3.org/TR/2021/WD-css-transforms-2-20211109/#individual-transform-serialization
String RotationStyleValue::to_string() const
{
    auto resolve_to_number = [](ValueComparingNonnullRefPtr<CSSStyleValue const> const& value) -> Optional<double> {
        if (value->is_number())
            return value->as_number().number();
        if (value->is_math() && value->as_math().resolves_to_number())
            return value->as_math().resolve_number();

        VERIFY_NOT_REACHED();
    };

    auto x_value = resolve_to_number(m_properties.rotation_x).value_or(0);
    auto y_value = resolve_to_number(m_properties.rotation_y).value_or(0);
    auto z_value = resolve_to_number(m_properties.rotation_z).value_or(0);

    // If the axis is parallel with the x or y axes, it must serialize as the appropriate keyword.
    if (x_value > 0.0 && y_value == 0 && z_value == 0)
        return MUST(String::formatted("x {}", m_properties.angle->to_string()));

    if (x_value == 0 && y_value > 0.0 && z_value == 0)
        return MUST(String::formatted("y {}", m_properties.angle->to_string()));

    // If a rotation about the z axis (that is, in 2D) is specified, the property must serialize as just an <angle>.
    if (x_value == 0 && y_value == 0 && z_value > 0.0)
        return m_properties.angle->to_string();

    // It must serialize as the keyword none if and only if none was originally specified.
    // NOTE: This is handled by returning a keyword from the parser.

    // If any other rotation is specified, the property must serialize with an axis specified.
    return MUST(String::formatted("{} {} {} {}", m_properties.rotation_x->to_string(), m_properties.rotation_y->to_string(), m_properties.rotation_z->to_string(), m_properties.angle->to_string()));
}

}

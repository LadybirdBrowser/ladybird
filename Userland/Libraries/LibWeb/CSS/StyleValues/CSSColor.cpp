/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSColor.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<CSSColor> CSSColor::create(StringView color_space, ValueComparingNonnullRefPtr<CSSStyleValue> c1, ValueComparingNonnullRefPtr<CSSStyleValue> c2, ValueComparingNonnullRefPtr<CSSStyleValue> c3, ValueComparingRefPtr<CSSStyleValue> alpha)
{
    VERIFY(any_of(s_supported_color_space, [=](auto supported) { return color_space == supported; }));

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    if (color_space == "xyz-d50")
        return adopt_ref(*new (nothrow) CSSColor(ColorType::XYZD50, move(c1), move(c2), move(c3), alpha.release_nonnull()));

    VERIFY_NOT_REACHED();
}

bool CSSColor::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_lab_like = verify_cast<CSSColor>(other_color);
    return m_properties == other_lab_like.m_properties;
}

// https://www.w3.org/TR/css-color-4/#serializing-color-function-values
String CSSColor::to_string() const
{
    // FIXME: Do this properly, taking unresolved calculated values into account.
    return serialize_a_srgb_value(to_color({}));
}

Color CSSColor::to_color(Optional<Layout::NodeWithStyle const&>) const
{
    auto const c1 = resolve_with_reference_value(m_properties.channels[0], 100).value_or(0);
    auto const c2 = resolve_with_reference_value(m_properties.channels[1], 100).value_or(0);
    auto const c3 = resolve_with_reference_value(m_properties.channels[2], 100).value_or(0);
    auto const alpha_val = resolve_alpha(m_properties.alpha).value_or(1);

    if (color_type() == ColorType::XYZD50)
        return Color::from_xyz50(c1, c2, c3, alpha_val);

    VERIFY_NOT_REACHED();
}

} // Web::CSS

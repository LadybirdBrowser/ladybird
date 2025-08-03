/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<AnchorStyleValue const> AnchorStyleValue::create(
    Optional<FlyString> const& anchor_name,
    ValueComparingNonnullRefPtr<CSSStyleValue const> const& anchor_side,
    ValueComparingRefPtr<CSSStyleValue const> const& fallback_value)
{
    return adopt_ref(*new (nothrow) AnchorStyleValue(anchor_name, anchor_side, fallback_value));
}

AnchorStyleValue::AnchorStyleValue(Optional<FlyString> const& anchor_name,
    ValueComparingNonnullRefPtr<CSSStyleValue const> const& anchor_side,
    ValueComparingRefPtr<CSSStyleValue const> const& fallback_value)
    : StyleValueWithDefaultOperators(Type::Anchor)
    , m_properties { .anchor_name = anchor_name, .anchor_side = anchor_side, .fallback_value = fallback_value }
{
}

String AnchorStyleValue::to_string(SerializationMode serialization_mode) const
{
    StringBuilder builder;
    builder.append("anchor("sv);

    if (anchor_name().has_value())
        builder.append(anchor_name().value());

    if (anchor_name().has_value())
        builder.append(' ');
    builder.append(anchor_side()->to_string(serialization_mode));

    if (fallback_value()) {
        builder.append(", "sv);
        builder.append(fallback_value()->to_string(serialization_mode));
    }

    builder.append(')');
    return MUST(builder.to_string());
}

}

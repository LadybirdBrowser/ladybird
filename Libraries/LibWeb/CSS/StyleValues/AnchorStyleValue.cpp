/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<AnchorStyleValue const> AnchorStyleValue::create(
    Optional<FlyString> const& anchor_name,
    ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
{
    return adopt_ref(*new (nothrow) AnchorStyleValue(anchor_name, anchor_side, fallback_value));
}

AnchorStyleValue::AnchorStyleValue(Optional<FlyString> const& anchor_name,
    ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
    : StyleValueWithDefaultOperators(Type::Anchor)
    , m_properties { .anchor_name = anchor_name, .anchor_side = anchor_side, .fallback_value = fallback_value }
{
}

void AnchorStyleValue::serialize(StringBuilder& builder, SerializationMode serialization_mode) const
{
    builder.append("anchor("sv);

    if (anchor_name().has_value())
        builder.append(anchor_name().value());

    if (anchor_name().has_value())
        builder.append(' ');
    anchor_side()->serialize(builder, serialization_mode);

    if (fallback_value()) {
        builder.append(", "sv);
        fallback_value()->serialize(builder, serialization_mode);
    }

    builder.append(')');
}

}

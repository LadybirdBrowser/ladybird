/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/AnchorSizeStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<AnchorSizeStyleValue const> AnchorSizeStyleValue::create(
    Optional<FlyString> const& anchor_name, Optional<AnchorSize> const& anchor_size,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
{
    return adopt_ref(*new (nothrow) AnchorSizeStyleValue(anchor_name, anchor_size, fallback_value));
}

AnchorSizeStyleValue::AnchorSizeStyleValue(Optional<FlyString> const& anchor_name, Optional<AnchorSize> const& anchor_size,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
    : StyleValueWithDefaultOperators(Type::AnchorSize)
    , m_properties { .anchor_name = anchor_name, .anchor_size = anchor_size, .fallback_value = fallback_value }
{
}

void AnchorSizeStyleValue::serialize(StringBuilder& builder, SerializationMode serialization_mode) const
{
    // FIXME: Handle SerializationMode.
    builder.append("anchor-size("sv);

    if (anchor_name().has_value())
        builder.append(anchor_name().value());

    if (anchor_size().has_value()) {
        if (anchor_name().has_value())
            builder.append(' ');
        builder.append(CSS::to_string(anchor_size().value()));
    }

    if (fallback_value()) {
        if (anchor_name().has_value() || anchor_size().has_value())
            builder.append(", "sv);
        fallback_value()->serialize(builder, serialization_mode);
    }

    builder.append(')');
}

}

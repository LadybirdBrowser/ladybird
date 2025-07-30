/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/AnchorSizeStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<AnchorSizeStyleValue const> AnchorSizeStyleValue::create(
    Optional<FlyString> const& anchor_name, Optional<AnchorSize> const& anchor_size,
    ValueComparingRefPtr<CSSStyleValue const> const& fallback_value)
{
    return adopt_ref(*new (nothrow) AnchorSizeStyleValue(anchor_name, anchor_size, fallback_value));
}

AnchorSizeStyleValue::AnchorSizeStyleValue(Optional<FlyString> const& anchor_name, Optional<AnchorSize> const& anchor_size,
    ValueComparingRefPtr<CSSStyleValue const> const& fallback_value)
    : StyleValueWithDefaultOperators(Type::AnchorSize)
    , m_properties { .anchor_name = anchor_name, .anchor_size = anchor_size, .fallback_value = fallback_value }
{
}

String AnchorSizeStyleValue::to_string(SerializationMode serialization_mode) const
{
    // FIXME: Handle SerializationMode.
    StringBuilder builder;
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
        builder.append(fallback_value()->to_string(serialization_mode));
    }

    builder.append(')');
    return MUST(builder.to_string());
}

}

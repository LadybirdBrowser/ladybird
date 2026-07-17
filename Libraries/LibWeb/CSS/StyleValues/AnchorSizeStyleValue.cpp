/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AnchorSizeStyleValue.h>

namespace Web::CSS {

static StyleValueFFI::StyleValueData* make_anchor_size_data(Optional<Utf16FlyString> const& anchor_name, Optional<AnchorSize> const& anchor_size, ValueComparingRefPtr<StyleValue const> const& fallback_value)
{
    // The Rust allocation takes ownership of one strong reference to the fallback value.
    if (fallback_value)
        fallback_value->ref();
    return StyleValueFFI::rust_style_value_create_anchor_size(
        anchor_name.has_value(),
        anchor_name.has_value() ? anchor_name->to_raw_leaked() : 0,
        anchor_size.has_value(),
        anchor_size.has_value() ? to_underlying(*anchor_size) : 0,
        fallback_value.ptr());
}

ValueComparingNonnullRefPtr<AnchorSizeStyleValue const> AnchorSizeStyleValue::create(
    Optional<Utf16FlyString> const& anchor_name, Optional<AnchorSize> const& anchor_size,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
{
    return adopt_ref(*new (nothrow) AnchorSizeStyleValue(anchor_name, anchor_size, fallback_value));
}

AnchorSizeStyleValue::AnchorSizeStyleValue(
    Optional<Utf16FlyString> const& anchor_name,
    Optional<AnchorSize> const& anchor_size,
    ValueComparingRefPtr<StyleValue const> const& fallback_value)
    : StyleValueWithDefaultOperators(Type::AnchorSize, make_anchor_size_data(anchor_name, anchor_size, fallback_value))
{
}

void AnchorSizeStyleValue::serialize(StringBuilder& builder, SerializationMode serialization_mode) const
{
    // FIXME: Handle SerializationMode.
    builder.append("anchor-size("sv);

    if (anchor_name().has_value())
        builder.append(serialize_an_identifier(anchor_name().value()));

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

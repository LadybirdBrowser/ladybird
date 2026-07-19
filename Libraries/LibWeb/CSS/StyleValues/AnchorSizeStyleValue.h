/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-anchor-position-1/#funcdef-anchor-size
class AnchorSizeStyleValue final : public StyleValueWithDefaultOperators<AnchorSizeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<AnchorSizeStyleValue const> create(Optional<Utf16FlyString> const& anchor_name,
        Optional<AnchorSize> const& anchor_size,
        ValueComparingRefPtr<StyleValue const> const& fallback_value);
    virtual ~AnchorSizeStyleValue() override = default;

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(AnchorSizeStyleValue const& other) const { return anchor_name() == other.anchor_name() && anchor_size() == other.anchor_size() && fallback_value() == other.fallback_value(); }

    Optional<Utf16FlyString> anchor_name() const
    {
        if (!m_value->anchor_size.has_anchor_name)
            return {};
        return Utf16FlyString::from_raw(m_value->anchor_size.anchor_name.raw);
    }
    Optional<AnchorSize> anchor_size() const
    {
        if (!m_value->anchor_size.has_anchor_size)
            return {};
        return static_cast<AnchorSize>(m_value->anchor_size.anchor_size);
    }
    ValueComparingRefPtr<StyleValue const> fallback_value() const
    {
        return static_cast<StyleValue const*>(m_value->anchor_size.fallback_value.pointer);
    }

private:
    AnchorSizeStyleValue(
        Optional<Utf16FlyString> const& anchor_name,
        Optional<AnchorSize> const& anchor_size,
        ValueComparingRefPtr<StyleValue const> const& fallback_value);
};

}

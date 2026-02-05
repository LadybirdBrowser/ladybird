/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-anchor-position-1/#funcdef-anchor-size
class AnchorSizeStyleValue final : public StyleValueWithDefaultOperators<AnchorSizeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<AnchorSizeStyleValue const> create(Optional<FlyString> const& anchor_name,
        Optional<AnchorSize> const& anchor_size,
        ValueComparingRefPtr<StyleValue const> const& fallback_value);
    virtual ~AnchorSizeStyleValue() override = default;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(AnchorSizeStyleValue const& other) const { return m_properties == other.m_properties; }

    Optional<FlyString const&> anchor_name() const { return m_properties.anchor_name; }
    Optional<AnchorSize> anchor_size() const { return m_properties.anchor_size; }
    ValueComparingRefPtr<StyleValue const> fallback_value() const
    {
        return m_properties.fallback_value;
    }

private:
    AnchorSizeStyleValue(Optional<FlyString> const& anchor_name, Optional<AnchorSize> const& anchor_size,
        ValueComparingRefPtr<StyleValue const> const& fallback_value);

    struct Properties {
        Optional<FlyString> anchor_name;
        Optional<AnchorSize> anchor_size;
        ValueComparingRefPtr<StyleValue const> fallback_value;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

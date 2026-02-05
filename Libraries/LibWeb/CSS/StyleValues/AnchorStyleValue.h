/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-anchor-position-1/#funcdef-anchor-size
class AnchorStyleValue final : public StyleValueWithDefaultOperators<AnchorStyleValue> {
public:
    static ValueComparingNonnullRefPtr<AnchorStyleValue const> create(Optional<FlyString> const& anchor_name,
        ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side,
        ValueComparingRefPtr<StyleValue const> const& fallback_value);
    virtual ~AnchorStyleValue() override = default;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(AnchorStyleValue const& other) const { return m_properties == other.m_properties; }

    Optional<FlyString const&> anchor_name() const { return m_properties.anchor_name; }
    ValueComparingNonnullRefPtr<StyleValue const> anchor_side() const
    {
        return m_properties.anchor_side;
    }
    ValueComparingRefPtr<StyleValue const> fallback_value() const
    {
        return m_properties.fallback_value;
    }

private:
    AnchorStyleValue(Optional<FlyString> const& anchor_name, ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side, ValueComparingRefPtr<StyleValue const> const& fallback_value);

    struct Properties {
        Optional<FlyString> anchor_name;
        ValueComparingNonnullRefPtr<StyleValue const> anchor_side;
        ValueComparingRefPtr<StyleValue const> fallback_value;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

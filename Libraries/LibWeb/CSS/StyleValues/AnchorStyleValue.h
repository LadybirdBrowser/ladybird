/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/AbstractNonMathCalcFunctionStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-anchor-position-1/#funcdef-anchor
class AnchorStyleValue final : public AbstractNonMathCalcFunctionStyleValue {
public:
    static ValueComparingNonnullRefPtr<AnchorStyleValue const> create(Optional<FlyString> const& anchor_name,
        ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side,
        ValueComparingRefPtr<StyleValue const> const& fallback_value);
    virtual ~AnchorStyleValue() override = default;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual RefPtr<CalculationNode const> resolve_to_calculation_node(CalculationContext const&, CalculationResolutionContext const&) const override;

    virtual bool equals(StyleValue const& other) const override;

    virtual bool is_computationally_independent() const override { return true; }

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

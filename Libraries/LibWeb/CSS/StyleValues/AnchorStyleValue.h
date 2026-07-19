/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/AbstractNonMathCalcFunctionStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-anchor-position-1/#funcdef-anchor
class AnchorStyleValue final : public AbstractNonMathCalcFunctionStyleValue {
public:
    static ValueComparingNonnullRefPtr<AnchorStyleValue const> create(Optional<Utf16FlyString> const& anchor_name,
        ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side,
        ValueComparingRefPtr<StyleValue const> const& fallback_value);
    virtual ~AnchorStyleValue() override = default;

    void serialize(StringBuilder&, SerializationMode) const;
    virtual RefPtr<CalculationNode const> resolve_to_calculation_node(CalculationContext const&, CalculationResolutionContext const&) const override;

    bool equals(StyleValue const& other) const;

    Optional<Utf16FlyString> anchor_name() const
    {
        if (!m_value->anchor.has_anchor_name)
            return {};
        return Utf16FlyString::from_raw(m_value->anchor.anchor_name.raw);
    }
    ValueComparingNonnullRefPtr<StyleValue const> anchor_side() const
    {
        return *static_cast<StyleValue const*>(m_value->anchor.anchor_side.pointer);
    }
    ValueComparingRefPtr<StyleValue const> fallback_value() const
    {
        return static_cast<StyleValue const*>(m_value->anchor.fallback_value.pointer);
    }

private:
    AnchorStyleValue(Optional<Utf16FlyString> const& anchor_name, ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side, ValueComparingRefPtr<StyleValue const> const& fallback_value);
};

}

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
    virtual ~AnchorStyleValue() override = default;

    virtual Optional<CalcNodeRef> resolve_to_calculation_node(CalculationContext const&, CalculationResolutionContext const&) const override;

    Optional<Utf16FlyString> anchor_name() const
    {
        if (!m_value->anchor.has_anchor_name)
            return {};
        return Utf16FlyString::from_raw(m_value->anchor.anchor_name.raw);
    }
    ValueComparingNonnullRefPtr<StyleValue const> anchor_side() const
    {
        return wrap_rust_child(m_value->anchor.anchor_side);
    }
    ValueComparingRefPtr<StyleValue const> fallback_value() const
    {
        return wrap_rust_child_or_null(m_value->anchor.fallback_value);
    }

private:
    friend class StyleValue;

    explicit AnchorStyleValue(StyleValueFFI::StyleValueData const* data)
        : AbstractNonMathCalcFunctionStyleValue(Type::Anchor, data)
    {
    }
};

}

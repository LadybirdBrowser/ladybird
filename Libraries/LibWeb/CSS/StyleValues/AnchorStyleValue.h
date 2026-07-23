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
    virtual Optional<CalcNodeRef> resolve_to_calculation_node(CalculationContext const&, CalculationResolutionContext const&) const override;

    bool equals(StyleValue const& other) const;

    Optional<Utf16FlyString> anchor_name() const
    {
        if (!m_value->anchor.has_anchor_name)
            return {};
        return Utf16FlyString::from_raw(m_value->anchor.anchor_name.raw);
    }
    ValueComparingNonnullRefPtr<StyleValue const> anchor_side() const
    {
        return m_anchor_side;
    }
    ValueComparingRefPtr<StyleValue const> fallback_value() const
    {
        return m_fallback_value;
    }

private:
    friend class StyleValue;

    explicit AnchorStyleValue(StyleValueFFI::StyleValueData const* data)
        : AbstractNonMathCalcFunctionStyleValue(Type::Anchor, data)
        , m_anchor_side(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->anchor.anchor_side.pointer))))
    {
        auto const* fallback_data = static_cast<StyleValueFFI::StyleValueData const*>(data->anchor.fallback_value.pointer);
        if (fallback_data)
            m_fallback_value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(fallback_data));
    }

    AnchorStyleValue(Optional<Utf16FlyString> const& anchor_name, ValueComparingNonnullRefPtr<StyleValue const> const& anchor_side, ValueComparingRefPtr<StyleValue const> const& fallback_value);

    ValueComparingNonnullRefPtr<StyleValue const> m_anchor_side;
    ValueComparingRefPtr<StyleValue const> m_fallback_value;
};

}

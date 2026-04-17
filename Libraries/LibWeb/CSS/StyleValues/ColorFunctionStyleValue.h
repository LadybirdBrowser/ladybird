/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <LibWeb/CSS/ColorFunctionDescriptor.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>

namespace Web::CSS {

class ColorFunctionStyleValue final : public ColorStyleValue {
public:
    static ValueComparingNonnullRefPtr<ColorFunctionStyleValue const> create(
        ColorType,
        ValueComparingNonnullRefPtr<StyleValue const> c1,
        ValueComparingNonnullRefPtr<StyleValue const> c2,
        ValueComparingNonnullRefPtr<StyleValue const> c3,
        ValueComparingRefPtr<StyleValue const> alpha = {},
        ColorSyntax = ColorSyntax::Modern,
        Optional<FlyString> name = {});

    virtual ~ColorFunctionStyleValue() override = default;

    StyleValue const& channel(size_t index) const { return *m_channels[index]; }
    StyleValue const& alpha() const { return *m_alpha; }
    Optional<FlyString> const& name() const { return m_name; }

    ColorFunctionDescriptor const& descriptor() const { return color_function_descriptor_for(*color_type()); }

    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual bool equals(StyleValue const&) const override;

    virtual bool is_computationally_independent() const override
    {
        return m_channels[0]->is_computationally_independent()
            && m_channels[1]->is_computationally_independent()
            && m_channels[2]->is_computationally_independent()
            && m_alpha->is_computationally_independent();
    }

    virtual bool is_color_function() const override { return true; }

    bool serializes_as_color_function() const
    {
        return descriptor().serialization_behavior == SerializationBehavior::ColorFunction;
    }

private:
    ColorFunctionStyleValue(
        ColorType color_type,
        ValueComparingNonnullRefPtr<StyleValue const> c1,
        ValueComparingNonnullRefPtr<StyleValue const> c2,
        ValueComparingNonnullRefPtr<StyleValue const> c3,
        ValueComparingNonnullRefPtr<StyleValue const> alpha,
        ColorSyntax color_syntax,
        Optional<FlyString> name)
        : ColorStyleValue(color_type, color_syntax)
        , m_channels { move(c1), move(c2), move(c3) }
        , m_alpha(move(alpha))
        , m_name(move(name))
    {
    }

    Array<ValueComparingNonnullRefPtr<StyleValue const>, 3> m_channels;
    ValueComparingNonnullRefPtr<StyleValue const> m_alpha;
    Optional<FlyString> m_name;
};

}

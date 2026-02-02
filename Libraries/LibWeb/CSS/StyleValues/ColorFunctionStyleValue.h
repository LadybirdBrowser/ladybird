/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>

namespace Web::CSS {

class ColorFunctionStyleValue final : public ColorStyleValue {
public:
    virtual ~ColorFunctionStyleValue() override = default;

    static ValueComparingNonnullRefPtr<ColorFunctionStyleValue const> create(StringView color_space, ValueComparingNonnullRefPtr<StyleValue const> c1, ValueComparingNonnullRefPtr<StyleValue const> c2, ValueComparingNonnullRefPtr<StyleValue const> c3, ValueComparingRefPtr<StyleValue const> alpha = {});

    virtual bool equals(StyleValue const&) const override;
    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    virtual bool is_color_function() const override { return true; }

    static constexpr Array s_supported_color_space = { "a98-rgb"sv, "display-p3"sv, "display-p3-linear"sv, "srgb"sv, "srgb-linear"sv, "prophoto-rgb"sv, "rec2020"sv, "xyz"sv, "xyz-d50"sv, "xyz-d65"sv };

private:
    ColorFunctionStyleValue(ColorType color_type, ValueComparingNonnullRefPtr<StyleValue const> c1, ValueComparingNonnullRefPtr<StyleValue const> c2, ValueComparingNonnullRefPtr<StyleValue const> c3, ValueComparingNonnullRefPtr<StyleValue const> alpha)
        : ColorStyleValue(color_type, ColorSyntax::Modern)
        , m_properties { .channels = { move(c1), move(c2), move(c3) }, .alpha = move(alpha) }
    {
    }

    struct Properties {
        Array<ValueComparingNonnullRefPtr<StyleValue const>, 3> channels;
        ValueComparingNonnullRefPtr<StyleValue const> alpha;
        bool operator==(Properties const&) const = default;
    };

    struct Resolved {
        Array<float, 3> channels {};
        float alpha {};
    };

    Optional<Resolved> resolve_properties(CalculationResolutionContext const& resolution_context) const;

    Properties m_properties;
};

} // Web::CSS

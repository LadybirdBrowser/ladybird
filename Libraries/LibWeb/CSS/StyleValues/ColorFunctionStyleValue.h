/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csscolor
class ColorFunctionStyleValue final : public CSSColorValue {
public:
    virtual ~ColorFunctionStyleValue() override = default;

    static ValueComparingNonnullRefPtr<ColorFunctionStyleValue const> create(StringView color_space, ValueComparingNonnullRefPtr<CSSStyleValue const> c1, ValueComparingNonnullRefPtr<CSSStyleValue const> c2, ValueComparingNonnullRefPtr<CSSStyleValue const> c3, ValueComparingRefPtr<CSSStyleValue const> alpha = {});

    virtual bool equals(CSSStyleValue const&) const override;
    virtual Optional<Color> to_color(Optional<Layout::NodeWithStyle const&>, CalculationResolutionContext const& resolution_context) const override;
    virtual String to_string(SerializationMode) const override;

    virtual bool is_color_function() const override { return true; }

    static constexpr Array s_supported_color_space = { "a98-rgb"sv, "display-p3"sv, "srgb"sv, "srgb-linear"sv, "prophoto-rgb"sv, "rec2020"sv, "xyz"sv, "xyz-d50"sv, "xyz-d65"sv };

private:
    ColorFunctionStyleValue(ColorType color_type, ValueComparingNonnullRefPtr<CSSStyleValue const> c1, ValueComparingNonnullRefPtr<CSSStyleValue const> c2, ValueComparingNonnullRefPtr<CSSStyleValue const> c3, ValueComparingNonnullRefPtr<CSSStyleValue const> alpha)
        : CSSColorValue(color_type, ColorSyntax::Modern)
        , m_properties { .channels = { move(c1), move(c2), move(c3) }, .alpha = move(alpha) }
    {
    }

    struct Properties {
        Array<ValueComparingNonnullRefPtr<CSSStyleValue const>, 3> channels;
        ValueComparingNonnullRefPtr<CSSStyleValue const> alpha;
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

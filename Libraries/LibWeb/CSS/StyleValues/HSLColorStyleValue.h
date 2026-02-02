/*
 * Copyright (c) 2024-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

class HSLColorStyleValue final : public ColorStyleValue {
public:
    static ValueComparingNonnullRefPtr<HSLColorStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> h, ValueComparingNonnullRefPtr<StyleValue const> s, ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingRefPtr<StyleValue const> alpha, ColorSyntax color_syntax)
    {
        // alpha defaults to 1
        if (!alpha)
            return adopt_ref(*new (nothrow) HSLColorStyleValue(move(h), move(s), move(l), NumberStyleValue::create(1), color_syntax));

        return adopt_ref(*new (nothrow) HSLColorStyleValue(move(h), move(s), move(l), alpha.release_nonnull(), color_syntax));
    }
    virtual ~HSLColorStyleValue() override = default;

    StyleValue const& h() const { return *m_properties.h; }
    StyleValue const& s() const { return *m_properties.s; }
    StyleValue const& l() const { return *m_properties.l; }
    StyleValue const& alpha() const { return *m_properties.alpha; }

    virtual Optional<Color> to_color(ColorResolutionContext color_resolution_context) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    virtual bool equals(StyleValue const& other) const override;

private:
    HSLColorStyleValue(ValueComparingNonnullRefPtr<StyleValue const> h, ValueComparingNonnullRefPtr<StyleValue const> s, ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingNonnullRefPtr<StyleValue const> alpha, ColorSyntax color_syntax)
        : ColorStyleValue(ColorType::HSL, color_syntax)
        , m_properties { .h = move(h), .s = move(s), .l = move(l), .alpha = move(alpha) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<StyleValue const> h;
        ValueComparingNonnullRefPtr<StyleValue const> s;
        ValueComparingNonnullRefPtr<StyleValue const> l;
        ValueComparingNonnullRefPtr<StyleValue const> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

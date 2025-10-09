/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

class RGBColorStyleValue final : public ColorStyleValue {
public:
    static ValueComparingNonnullRefPtr<RGBColorStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> r, ValueComparingNonnullRefPtr<StyleValue const> g, ValueComparingNonnullRefPtr<StyleValue const> b, ValueComparingRefPtr<StyleValue const> alpha, ColorSyntax color_syntax, Optional<FlyString> const& name = {})
    {
        // alpha defaults to 1
        if (!alpha)
            return adopt_ref(*new (nothrow) RGBColorStyleValue(move(r), move(g), move(b), NumberStyleValue::create(1), color_syntax, name));

        return adopt_ref(*new (nothrow) RGBColorStyleValue(move(r), move(g), move(b), alpha.release_nonnull(), color_syntax, name));
    }
    virtual ~RGBColorStyleValue() override = default;

    StyleValue const& r() const { return *m_properties.r; }
    StyleValue const& g() const { return *m_properties.g; }
    StyleValue const& b() const { return *m_properties.b; }
    StyleValue const& alpha() const { return *m_properties.alpha; }

    virtual Optional<Color> to_color(ColorResolutionContext) const override;

    virtual String to_string(SerializationMode) const override;

    virtual bool equals(StyleValue const& other) const override;

private:
    RGBColorStyleValue(ValueComparingNonnullRefPtr<StyleValue const> r, ValueComparingNonnullRefPtr<StyleValue const> g, ValueComparingNonnullRefPtr<StyleValue const> b, ValueComparingNonnullRefPtr<StyleValue const> alpha, ColorSyntax color_syntax, Optional<FlyString> name = {})
        : ColorStyleValue(ColorType::RGB, color_syntax)
        , m_properties { .r = move(r), .g = move(g), .b = move(b), .alpha = move(alpha), .name = move(name) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<StyleValue const> r;
        ValueComparingNonnullRefPtr<StyleValue const> g;
        ValueComparingNonnullRefPtr<StyleValue const> b;
        ValueComparingNonnullRefPtr<StyleValue const> alpha;
        Optional<FlyString> name;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

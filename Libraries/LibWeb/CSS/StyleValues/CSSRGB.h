/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssrgb
class CSSRGB final : public CSSColorValue {
public:
    static ValueComparingNonnullRefPtr<CSSRGB> create(ValueComparingNonnullRefPtr<CSSStyleValue> r, ValueComparingNonnullRefPtr<CSSStyleValue> g, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingRefPtr<CSSStyleValue> alpha, ColorSyntax color_syntax, Optional<FlyString> name = {})
    {
        // alpha defaults to 1
        if (!alpha)
            return adopt_ref(*new (nothrow) CSSRGB(move(r), move(g), move(b), NumberStyleValue::create(1), color_syntax, name));

        return adopt_ref(*new (nothrow) CSSRGB(move(r), move(g), move(b), alpha.release_nonnull(), color_syntax, name));
    }
    virtual ~CSSRGB() override = default;

    CSSStyleValue const& r() const { return *m_properties.r; }
    CSSStyleValue const& g() const { return *m_properties.g; }
    CSSStyleValue const& b() const { return *m_properties.b; }
    CSSStyleValue const& alpha() const { return *m_properties.alpha; }

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;

    virtual String to_string(SerializationMode) const override;

    virtual bool equals(CSSStyleValue const& other) const override;

private:
    CSSRGB(ValueComparingNonnullRefPtr<CSSStyleValue> r, ValueComparingNonnullRefPtr<CSSStyleValue> g, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingNonnullRefPtr<CSSStyleValue> alpha, ColorSyntax color_syntax, Optional<FlyString> name = {})
        : CSSColorValue(ColorType::RGB, color_syntax)
        , m_properties { .r = move(r), .g = move(g), .b = move(b), .alpha = move(alpha), .name = name }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue> r;
        ValueComparingNonnullRefPtr<CSSStyleValue> g;
        ValueComparingNonnullRefPtr<CSSStyleValue> b;
        ValueComparingNonnullRefPtr<CSSStyleValue> alpha;
        Optional<FlyString> name;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

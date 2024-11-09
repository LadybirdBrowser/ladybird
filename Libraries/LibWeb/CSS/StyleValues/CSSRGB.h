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
    static ValueComparingNonnullRefPtr<CSSRGB> create(ValueComparingNonnullRefPtr<CSSStyleValue> r, ValueComparingNonnullRefPtr<CSSStyleValue> g, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingRefPtr<CSSStyleValue> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            return adopt_ref(*new (nothrow) CSSRGB(move(r), move(g), move(b), NumberStyleValue::create(1)));

        return adopt_ref(*new (nothrow) CSSRGB(move(r), move(g), move(b), alpha.release_nonnull()));
    }
    virtual ~CSSRGB() override = default;

    CSSStyleValue const& r() const { return *m_properties.r; }
    CSSStyleValue const& g() const { return *m_properties.g; }
    CSSStyleValue const& b() const { return *m_properties.b; }
    CSSStyleValue const& alpha() const { return *m_properties.alpha; }

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;

    String to_string() const override;

    virtual bool equals(CSSStyleValue const& other) const override;

private:
    CSSRGB(ValueComparingNonnullRefPtr<CSSStyleValue> r, ValueComparingNonnullRefPtr<CSSStyleValue> g, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSColorValue(ColorType::RGB)
        , m_properties { .r = move(r), .g = move(g), .b = move(b), .alpha = move(alpha) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue> r;
        ValueComparingNonnullRefPtr<CSSStyleValue> g;
        ValueComparingNonnullRefPtr<CSSStyleValue> b;
        ValueComparingNonnullRefPtr<CSSStyleValue> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssoklab
class CSSOKLab final : public CSSColorValue {
public:
    static ValueComparingNonnullRefPtr<CSSOKLab> create(ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> a, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingRefPtr<CSSStyleValue> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            return adopt_ref(*new (nothrow) CSSOKLab(move(l), move(a), move(b), NumberStyleValue::create(1)));

        return adopt_ref(*new (nothrow) CSSOKLab(move(l), move(a), move(b), alpha.release_nonnull()));
    }
    virtual ~CSSOKLab() override = default;

    CSSStyleValue const& l() const { return *m_properties.l; }
    CSSStyleValue const& a() const { return *m_properties.a; }
    CSSStyleValue const& b() const { return *m_properties.b; }
    CSSStyleValue const& alpha() const { return *m_properties.alpha; }

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;

    String to_string() const override;

    virtual bool equals(CSSStyleValue const& other) const override;

private:
    CSSOKLab(ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> a, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSColorValue(ColorType::OKLab)
        , m_properties { .l = move(l), .a = move(a), .b = move(b), .alpha = move(alpha) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue> l;
        ValueComparingNonnullRefPtr<CSSStyleValue> a;
        ValueComparingNonnullRefPtr<CSSStyleValue> b;
        ValueComparingNonnullRefPtr<CSSStyleValue> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssoklch
class CSSOKLCH final : public CSSColorValue {
public:
    static ValueComparingNonnullRefPtr<CSSOKLCH> create(ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> c, ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingRefPtr<CSSStyleValue> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            return adopt_ref(*new (nothrow) CSSOKLCH(move(l), move(c), move(h), NumberStyleValue::create(1)));

        return adopt_ref(*new (nothrow) CSSOKLCH(move(l), move(c), move(h), alpha.release_nonnull()));
    }
    virtual ~CSSOKLCH() override = default;

    CSSStyleValue const& l() const { return *m_properties.l; }
    CSSStyleValue const& c() const { return *m_properties.c; }
    CSSStyleValue const& h() const { return *m_properties.h; }
    CSSStyleValue const& alpha() const { return *m_properties.alpha; }

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;

    String to_string() const override;

    virtual bool equals(CSSStyleValue const& other) const override;

private:
    CSSOKLCH(ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> c, ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSColorValue(ColorType::OKLCH)
        , m_properties { .l = move(l), .c = move(c), .h = move(h), .alpha = move(alpha) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue> l;
        ValueComparingNonnullRefPtr<CSSStyleValue> c;
        ValueComparingNonnullRefPtr<CSSStyleValue> h;
        ValueComparingNonnullRefPtr<CSSStyleValue> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

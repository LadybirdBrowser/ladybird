/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csshsl
class CSSHSL final : public CSSColorValue {
public:
    static ValueComparingNonnullRefPtr<CSSHSL> create(ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingNonnullRefPtr<CSSStyleValue> s, ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingRefPtr<CSSStyleValue> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            return adopt_ref(*new (nothrow) CSSHSL(move(h), move(s), move(l), NumberStyleValue::create(1)));

        return adopt_ref(*new (nothrow) CSSHSL(move(h), move(s), move(l), alpha.release_nonnull()));
    }
    virtual ~CSSHSL() override = default;

    CSSStyleValue const& h() const { return *m_properties.h; }
    CSSStyleValue const& s() const { return *m_properties.s; }
    CSSStyleValue const& l() const { return *m_properties.l; }
    CSSStyleValue const& alpha() const { return *m_properties.alpha; }

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;

    String to_string() const override;

    virtual bool equals(CSSStyleValue const& other) const override;

private:
    CSSHSL(ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingNonnullRefPtr<CSSStyleValue> s, ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSColorValue(ColorType::HSL)
        , m_properties { .h = move(h), .s = move(s), .l = move(l), .alpha = move(alpha) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue> h;
        ValueComparingNonnullRefPtr<CSSStyleValue> s;
        ValueComparingNonnullRefPtr<CSSStyleValue> l;
        ValueComparingNonnullRefPtr<CSSStyleValue> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csshwb
class CSSHWB final : public CSSColorValue {
public:
    static ValueComparingNonnullRefPtr<CSSHWB> create(ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingNonnullRefPtr<CSSStyleValue> w, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingRefPtr<CSSStyleValue> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            return adopt_ref(*new (nothrow) CSSHWB(move(h), move(w), move(b), NumberStyleValue::create(1)));

        return adopt_ref(*new (nothrow) CSSHWB(move(h), move(w), move(b), alpha.release_nonnull()));
    }
    virtual ~CSSHWB() override = default;

    CSSStyleValue const& h() const { return *m_properties.h; }
    CSSStyleValue const& w() const { return *m_properties.w; }
    CSSStyleValue const& b() const { return *m_properties.b; }
    CSSStyleValue const& alpha() const { return *m_properties.alpha; }

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;

    String to_string() const override;

    virtual bool equals(CSSStyleValue const& other) const override;

private:
    CSSHWB(ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingNonnullRefPtr<CSSStyleValue> w, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSColorValue(ColorType::HWB)
        , m_properties { .h = move(h), .w = move(w), .b = move(b), .alpha = move(alpha) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue> h;
        ValueComparingNonnullRefPtr<CSSStyleValue> w;
        ValueComparingNonnullRefPtr<CSSStyleValue> b;
        ValueComparingNonnullRefPtr<CSSStyleValue> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

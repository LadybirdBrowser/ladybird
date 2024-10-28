/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

class CSSLCHLike : public CSSColorValue {
public:
    template<DerivedFrom<CSSLCHLike> T>
    static ValueComparingNonnullRefPtr<T> create(ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> c, ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingRefPtr<CSSStyleValue> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            alpha = NumberStyleValue::create(1);

        return adopt_ref(*new (nothrow) T({}, move(l), move(c), move(h), alpha.release_nonnull()));
    }
    virtual ~CSSLCHLike() override = default;

    CSSStyleValue const& l() const { return *m_properties.l; }
    CSSStyleValue const& c() const { return *m_properties.c; }
    CSSStyleValue const& h() const { return *m_properties.h; }
    CSSStyleValue const& alpha() const { return *m_properties.alpha; }

    virtual bool equals(CSSStyleValue const& other) const override;

protected:
    CSSLCHLike(ColorType color_type, ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> c, ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSColorValue(color_type)
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

// https://drafts.css-houdini.org/css-typed-om-1/#csslch
class CSSLCH final : public CSSLCHLike {
public:
    CSSLCH(Badge<CSSLCHLike>, ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> c, ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSLCHLike(ColorType::LCH, move(l), move(c), move(h), move(alpha))
    {
    }
    virtual ~CSSLCH() override = default;

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;

    virtual String to_string() const override;
};

// https://drafts.css-houdini.org/css-typed-om-1/#cssoklch
class CSSOKLCH final : public CSSLCHLike {
public:
    CSSOKLCH(Badge<CSSLCHLike>, ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> c, ValueComparingNonnullRefPtr<CSSStyleValue> h, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSLCHLike(ColorType::OKLCH, move(l), move(c), move(h), move(alpha))
    {
    }
    virtual ~CSSOKLCH() override = default;

    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;

    String to_string() const override;
};

}

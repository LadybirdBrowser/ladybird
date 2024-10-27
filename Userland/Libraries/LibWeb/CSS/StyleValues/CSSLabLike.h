/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

class CSSLabLike : public CSSColorValue {
public:
    template<typename T>
    static ValueComparingNonnullRefPtr<T> create(ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> a, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingRefPtr<CSSStyleValue> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            alpha = NumberStyleValue::create(1);

        return adopt_ref(*new (nothrow) T({}, move(l), move(a), move(b), alpha.release_nonnull()));
    }

    virtual ~CSSLabLike() override = default;

    CSSStyleValue const& l() const { return *m_properties.l; }
    CSSStyleValue const& a() const { return *m_properties.a; }
    CSSStyleValue const& b() const { return *m_properties.b; }
    CSSStyleValue const& alpha() const { return *m_properties.alpha; }

    virtual bool equals(CSSStyleValue const& other) const override;

protected:
    CSSLabLike(ColorType color_type, ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> a, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSColorValue(color_type)
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

// https://drafts.css-houdini.org/css-typed-om-1/#cssoklab
class CSSOKLab final : public CSSLabLike {
public:
    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;
    virtual String to_string() const override;

    CSSOKLab(Badge<CSSLabLike>, ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> a, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSLabLike(ColorType::OKLab, move(l), move(a), move(b), move(alpha))
    {
    }
};

// https://drafts.css-houdini.org/css-typed-om-1/#csslab
class CSSLab final : public CSSLabLike {
public:
    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;
    virtual String to_string() const override;

    CSSLab(Badge<CSSLabLike>, ValueComparingNonnullRefPtr<CSSStyleValue> l, ValueComparingNonnullRefPtr<CSSStyleValue> a, ValueComparingNonnullRefPtr<CSSStyleValue> b, ValueComparingNonnullRefPtr<CSSStyleValue> alpha)
        : CSSLabLike(ColorType::Lab, move(l), move(a), move(b), move(alpha))
    {
    }
};

}

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

class LCHLikeColorStyleValue : public ColorStyleValue {
public:
    template<DerivedFrom<LCHLikeColorStyleValue> T>
    static ValueComparingNonnullRefPtr<T const> create(ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingNonnullRefPtr<StyleValue const> c, ValueComparingNonnullRefPtr<StyleValue const> h, ValueComparingRefPtr<StyleValue const> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            alpha = NumberStyleValue::create(1);

        return adopt_ref(*new (nothrow) T({}, move(l), move(c), move(h), alpha.release_nonnull()));
    }
    virtual ~LCHLikeColorStyleValue() override = default;

    StyleValue const& l() const { return *m_properties.l; }
    StyleValue const& c() const { return *m_properties.c; }
    StyleValue const& h() const { return *m_properties.h; }
    StyleValue const& alpha() const { return *m_properties.alpha; }

    virtual bool equals(StyleValue const& other) const override;

protected:
    LCHLikeColorStyleValue(ColorType color_type, ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingNonnullRefPtr<StyleValue const> c, ValueComparingNonnullRefPtr<StyleValue const> h, ValueComparingNonnullRefPtr<StyleValue const> alpha)
        : ColorStyleValue(color_type, ColorSyntax::Modern)
        , m_properties { .l = move(l), .c = move(c), .h = move(h), .alpha = move(alpha) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<StyleValue const> l;
        ValueComparingNonnullRefPtr<StyleValue const> c;
        ValueComparingNonnullRefPtr<StyleValue const> h;
        ValueComparingNonnullRefPtr<StyleValue const> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

class LCHColorStyleValue final : public LCHLikeColorStyleValue {
public:
    LCHColorStyleValue(Badge<LCHLikeColorStyleValue>, ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingNonnullRefPtr<StyleValue const> c, ValueComparingNonnullRefPtr<StyleValue const> h, ValueComparingNonnullRefPtr<StyleValue const> alpha)
        : LCHLikeColorStyleValue(ColorType::LCH, move(l), move(c), move(h), move(alpha))
    {
    }
    virtual ~LCHColorStyleValue() override = default;

    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
};

class OKLCHColorStyleValue final : public LCHLikeColorStyleValue {
public:
    OKLCHColorStyleValue(Badge<LCHLikeColorStyleValue>, ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingNonnullRefPtr<StyleValue const> c, ValueComparingNonnullRefPtr<StyleValue const> h, ValueComparingNonnullRefPtr<StyleValue const> alpha)
        : LCHLikeColorStyleValue(ColorType::OKLCH, move(l), move(c), move(h), move(alpha))
    {
    }
    virtual ~OKLCHColorStyleValue() override = default;

    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
};

}

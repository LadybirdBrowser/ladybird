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

class LabLikeColorStyleValue : public ColorStyleValue {
public:
    template<typename T>
    static ValueComparingNonnullRefPtr<T const> create(ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingNonnullRefPtr<StyleValue const> a, ValueComparingNonnullRefPtr<StyleValue const> b, ValueComparingRefPtr<StyleValue const> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            alpha = NumberStyleValue::create(1);

        return adopt_ref(*new (nothrow) T({}, move(l), move(a), move(b), alpha.release_nonnull()));
    }

    virtual ~LabLikeColorStyleValue() override = default;

    StyleValue const& l() const { return *m_properties.l; }
    StyleValue const& a() const { return *m_properties.a; }
    StyleValue const& b() const { return *m_properties.b; }
    StyleValue const& alpha() const { return *m_properties.alpha; }

    virtual bool equals(StyleValue const& other) const override;

protected:
    LabLikeColorStyleValue(ColorType color_type, ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingNonnullRefPtr<StyleValue const> a, ValueComparingNonnullRefPtr<StyleValue const> b, ValueComparingNonnullRefPtr<StyleValue const> alpha)
        : ColorStyleValue(color_type, ColorSyntax::Modern)
        , m_properties { .l = move(l), .a = move(a), .b = move(b), .alpha = move(alpha) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<StyleValue const> l;
        ValueComparingNonnullRefPtr<StyleValue const> a;
        ValueComparingNonnullRefPtr<StyleValue const> b;
        ValueComparingNonnullRefPtr<StyleValue const> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

class OKLabColorStyleValue final : public LabLikeColorStyleValue {
public:
    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    OKLabColorStyleValue(Badge<LabLikeColorStyleValue>, ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingNonnullRefPtr<StyleValue const> a, ValueComparingNonnullRefPtr<StyleValue const> b, ValueComparingNonnullRefPtr<StyleValue const> alpha)
        : LabLikeColorStyleValue(ColorType::OKLab, move(l), move(a), move(b), move(alpha))
    {
    }
};

class LabColorStyleValue final : public LabLikeColorStyleValue {
public:
    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    LabColorStyleValue(Badge<LabLikeColorStyleValue>, ValueComparingNonnullRefPtr<StyleValue const> l, ValueComparingNonnullRefPtr<StyleValue const> a, ValueComparingNonnullRefPtr<StyleValue const> b, ValueComparingNonnullRefPtr<StyleValue const> alpha)
        : LabLikeColorStyleValue(ColorType::Lab, move(l), move(a), move(b), move(alpha))
    {
    }
};

}

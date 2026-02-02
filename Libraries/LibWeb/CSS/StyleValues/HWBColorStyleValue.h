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

class HWBColorStyleValue final : public ColorStyleValue {
public:
    static ValueComparingNonnullRefPtr<HWBColorStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> h, ValueComparingNonnullRefPtr<StyleValue const> w, ValueComparingNonnullRefPtr<StyleValue const> b, ValueComparingRefPtr<StyleValue const> alpha = {})
    {
        // alpha defaults to 1
        if (!alpha)
            return adopt_ref(*new (nothrow) HWBColorStyleValue(move(h), move(w), move(b), NumberStyleValue::create(1)));

        return adopt_ref(*new (nothrow) HWBColorStyleValue(move(h), move(w), move(b), alpha.release_nonnull()));
    }
    virtual ~HWBColorStyleValue() override = default;

    StyleValue const& h() const { return *m_properties.h; }
    StyleValue const& w() const { return *m_properties.w; }
    StyleValue const& b() const { return *m_properties.b; }
    StyleValue const& alpha() const { return *m_properties.alpha; }

    virtual Optional<Color> to_color(ColorResolutionContext color_resolution_context) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    virtual bool equals(StyleValue const& other) const override;

private:
    HWBColorStyleValue(ValueComparingNonnullRefPtr<StyleValue const> h, ValueComparingNonnullRefPtr<StyleValue const> w, ValueComparingNonnullRefPtr<StyleValue const> b, ValueComparingNonnullRefPtr<StyleValue const> alpha)
        : ColorStyleValue(ColorType::HWB, ColorSyntax::Modern)
        , m_properties { .h = move(h), .w = move(w), .b = move(b), .alpha = move(alpha) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<StyleValue const> h;
        ValueComparingNonnullRefPtr<StyleValue const> w;
        ValueComparingNonnullRefPtr<StyleValue const> b;
        ValueComparingNonnullRefPtr<StyleValue const> alpha;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}

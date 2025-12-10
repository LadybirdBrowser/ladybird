/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class RadialSizeStyleValue : public StyleValueWithDefaultOperators<RadialSizeStyleValue> {
public:
    using Component = Variant<RadialExtent, NonnullRefPtr<StyleValue const>>;
    static ValueComparingNonnullRefPtr<RadialSizeStyleValue const> create(Vector<Component> components)
    {
        VERIFY(components.size() == 1 || components.size() == 2);
        return adopt_ref(*new (nothrow) RadialSizeStyleValue(move(components)));
    }

    virtual ~RadialSizeStyleValue() override = default;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual String to_string(SerializationMode serialization_mode) const override;

    Vector<Component> components() const { return m_components; }

    CSSPixels resolve_circle_size(CSSPixelPoint const& center, CSSPixelRect const& reference_box, Layout::Node const&) const;
    CSSPixelSize resolve_ellipse_size(CSSPixelPoint const& center, CSSPixelRect const& reference_box, Layout::Node const&) const;

    bool properties_equal(RadialSizeStyleValue const& other) const { return m_components == other.m_components; }

private:
    explicit RadialSizeStyleValue(Vector<Component> components)
        : StyleValueWithDefaultOperators(Type::RadialSize)
        , m_components(move(components))
    {
    }

    Vector<Component> m_components;
};

}

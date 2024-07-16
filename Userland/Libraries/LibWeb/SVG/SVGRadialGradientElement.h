/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGGradientElement.h>

namespace Web::SVG {

class SVGRadialGradientElement : public SVGGradientElement {
    WEB_PLATFORM_OBJECT(SVGRadialGradientElement, SVGGradientElement);
    JS_DECLARE_ALLOCATOR(SVGRadialGradientElement);

public:
    virtual ~SVGRadialGradientElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    virtual Optional<Painting::PaintStyle> to_gfx_paint_style(SVGPaintContext const&) const override;

    JS::NonnullGCPtr<SVGAnimatedLength> cx() const;
    JS::NonnullGCPtr<SVGAnimatedLength> cy() const;
    JS::NonnullGCPtr<SVGAnimatedLength> fx() const;
    JS::NonnullGCPtr<SVGAnimatedLength> fy() const;
    JS::NonnullGCPtr<SVGAnimatedLength> fr() const;
    JS::NonnullGCPtr<SVGAnimatedLength> r() const;

protected:
    SVGRadialGradientElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    JS::GCPtr<SVGRadialGradientElement const> linked_radial_gradient(HashTable<SVGGradientElement const*>& seen_gradients) const
    {
        if (auto gradient = linked_gradient(seen_gradients); gradient && is<SVGRadialGradientElement>(*gradient))
            return &verify_cast<SVGRadialGradientElement>(*gradient);
        return {};
    }

    NumberPercentage start_circle_x() const;
    NumberPercentage start_circle_y() const;
    NumberPercentage start_circle_radius() const;
    NumberPercentage end_circle_x() const;
    NumberPercentage end_circle_y() const;
    NumberPercentage end_circle_radius() const;

    NumberPercentage start_circle_x_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage start_circle_y_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage start_circle_radius_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_circle_x_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_circle_y_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_circle_radius_impl(HashTable<SVGGradientElement const*>& seen_gradients) const;

    Optional<NumberPercentage> m_cx;
    Optional<NumberPercentage> m_cy;
    Optional<NumberPercentage> m_fx;
    Optional<NumberPercentage> m_fy;
    Optional<NumberPercentage> m_fr;
    Optional<NumberPercentage> m_r;

    mutable RefPtr<Painting::SVGRadialGradientPaintStyle> m_paint_style;
};

}

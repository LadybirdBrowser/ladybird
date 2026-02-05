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
    GC_DECLARE_ALLOCATOR(SVGRadialGradientElement);

public:
    virtual ~SVGRadialGradientElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    virtual Optional<Painting::PaintStyle> to_gfx_paint_style(SVGPaintContext const&) const override;

    GC::Ref<SVGAnimatedLength> cx() const;
    GC::Ref<SVGAnimatedLength> cy() const;
    GC::Ref<SVGAnimatedLength> fx() const;
    GC::Ref<SVGAnimatedLength> fy() const;
    GC::Ref<SVGAnimatedLength> fr() const;
    GC::Ref<SVGAnimatedLength> r() const;

protected:
    SVGRadialGradientElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    GC::Ptr<SVGRadialGradientElement const> linked_radial_gradient(HashTable<SVGGradientElement const*>& seen_gradients) const
    {
        if (auto gradient = linked_gradient(seen_gradients); gradient && is<SVGRadialGradientElement>(*gradient))
            return &as<SVGRadialGradientElement>(*gradient);
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

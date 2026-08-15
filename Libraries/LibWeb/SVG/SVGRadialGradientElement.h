/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGradientElement.h>

namespace Web::SVG {

class SVGRadialGradientElement : public SVGGradientElement {
    WEB_WRAPPABLE(SVGRadialGradientElement, SVGGradientElement);
    GC_DECLARE_ALLOCATOR(SVGRadialGradientElement);

public:
    virtual ~SVGRadialGradientElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Optional<Painting::PaintStyle> to_gfx_paint_style(SVGPaintContext const&) const override;

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGRadialGradientElement__cx
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(cx, Horizontal, SVGLengthValue::percentage(50));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGRadialGradientElement__cy
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(cy, Vertical, SVGLengthValue::percentage(50));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGRadialGradientElement__fx
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(fx, Horizontal, SVGLengthValue::percentage(50));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGRadialGradientElement__fy
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(fy, Vertical, SVGLengthValue::percentage(50));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGRadialGradientElement__fr
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(fr, Unspecified, SVGLengthValue::percentage(0));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGRadialGradientElement__r
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(r, Unspecified, SVGLengthValue::percentage(50));

protected:
    SVGRadialGradientElement(DOM::Document&, DOM::QualifiedName);

private:
    GC::Ptr<SVGRadialGradientElement const> linked_radial_gradient(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const
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

    NumberPercentage start_circle_x_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage start_circle_y_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage start_circle_radius_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_circle_x_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_circle_y_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;
    NumberPercentage end_circle_radius_impl(GC::RootHashTable<SVGGradientElement const*>& seen_gradients) const;

    Optional<NumberPercentage> m_cx;
    Optional<NumberPercentage> m_cy;
    Optional<NumberPercentage> m_fx;
    Optional<NumberPercentage> m_fy;
    Optional<NumberPercentage> m_fr;
    Optional<NumberPercentage> m_r;
};

}

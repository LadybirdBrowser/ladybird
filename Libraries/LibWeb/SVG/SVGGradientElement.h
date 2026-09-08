/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IterationDecision.h>
#include <LibGC/RootHashTable.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Color.h>
#include <LibGfx/InterpolationColorSpace.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGStopElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

struct SVGPaintContext {
    Gfx::FloatRect viewport;
    Gfx::FloatRect path_bounding_box;
    Gfx::AffineTransform paint_transform;
    Gfx::FloatSize content_scale;
};

struct GradientAttributes {
    Optional<NumberPercentage> x1, y1, x2, y2;
    Optional<NumberPercentage> cx, cy, r, fx, fy, fr;
};

// Template attributes and styled stops, independent of the geometry using the gradient.
struct ResolvedGradient {
    bool is_radial { false };
    GradientUnits units { GradientUnits::ObjectBoundingBox };
    SpreadMethod spread_method { SpreadMethod::Pad };
    Gfx::AffineTransform transform;
    Gfx::InterpolationColorSpace color_space { Gfx::InterpolationColorSpace::SRGB };
    NumberPercentage start_x { 0, false };
    NumberPercentage start_y { 0, false };
    NumberPercentage end_x { 0, false };
    NumberPercentage end_y { 0, false };
    NumberPercentage start_radius { 0, false };
    NumberPercentage end_radius { 0, false };
    struct Stop {
        float offset;
        Gfx::Color color;
    };
    Vector<Stop> stops;
};

class SVGGradientElement
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_WRAPPABLE(SVGGradientElement, SVGElement);

public:
    virtual ~SVGGradientElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    ResolvedGradient resolve_gradient() const;

protected:
    SVGGradientElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool is_radial_gradient() const = 0;
    virtual void collect_gradient_attributes(GradientAttributes&) const = 0;

private:
    virtual bool is_svg_gradient_element() const final { return true; }

    GC::Ptr<SVGGradientElement const> linked_gradient() const;

    // https://svgwg.org/svg2-draft/pservers.html#LinearGradientAttributes
    Optional<GradientUnits> m_gradient_units = {};

    Optional<SpreadMethod> m_spread_method = {};
    Optional<Gfx::AffineTransform> m_gradient_transform = {};
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGGradientElement>() const { return is_svg_gradient_element(); }

}

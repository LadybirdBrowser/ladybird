/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/PaintStyle.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedTransformList.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct SVGBoundingBoxOptions;

}

namespace Web::SVG {

class WEB_API SVGGraphicsElement : public SVGElement {
    WEB_WRAPPABLE(SVGGraphicsElement, SVGElement);

public:
    Optional<Gfx::Color> fill_color() const;
    Optional<Gfx::Color> stroke_color() const;
    Vector<float> stroke_dasharray() const;
    Optional<float> stroke_dashoffset() const;
    Optional<float> stroke_width() const;
    Optional<float> fill_opacity() const;
    CSS::PaintOrderList paint_order() const;
    Optional<CSS::StrokeLinecap> stroke_linecap() const;
    Optional<CSS::StrokeLinejoin> stroke_linejoin() const;
    Optional<double> stroke_miterlimit() const;
    Optional<float> stroke_opacity() const;
    Optional<FillRule> fill_rule() const;
    Optional<ClipRule> clip_rule() const;

    virtual Optional<ViewBox> active_view_box() const
    {
        if (auto* svg_fit_to_view_box = as_if<SVGFitToViewBox>(*this))
            return svg_fit_to_view_box->view_box();
        return {};
    }

    float visible_stroke_width() const
    {
        if (auto color = stroke_color(); color.has_value() && color->alpha() > 0)
            return stroke_width().value_or(0);
        return 0;
    }

    GC::Ptr<SVG::SVGMaskElement const> mask() const;
    GC::Ptr<SVG::SVGClipPathElement const> clip_path() const;

    GC::Ptr<SVG::SVGPatternElement const> fill_pattern() const;
    GC::Ptr<SVG::SVGPatternElement const> stroke_pattern() const;

    WebIDL::ExceptionOr<GC::Ref<Geometry::DOMRect>> get_b_box(Bindings::SVGBoundingBoxOptions const&);
    GC::Ref<SVGAnimatedTransformList> transform() const;

    GC::Ptr<Geometry::DOMMatrix> get_ctm();
    GC::Ptr<Geometry::DOMMatrix> get_screen_ctm();

    // The transform property carries the transform attribute through the cascade; this is the
    // extra transformation some elements apply beyond it, such as the x/y translation of <use>.
    virtual Gfx::AffineTransform additional_element_transform() const
    {
        return {};
    }

    struct PatternPaintServer {
        Painting::Paintable const* pattern_paintable;
        Gfx::FloatRect tile_rect;
        Gfx::FloatSize content_scale;
        Painting::TransformData tile_content_transform;
        Optional<Gfx::AffineTransform> device_pattern_transform;
    };
    using PaintServer = Variant<Painting::PaintStyle, PatternPaintServer>;
    Optional<PaintServer> fill_paint_server(SVGPaintContext const&, double device_pixels_per_css_pixel) const;
    Optional<PaintServer> stroke_paint_server(SVGPaintContext const&, double device_pixels_per_css_pixel) const;

protected:
    SVGGraphicsElement(DOM::Document&, DOM::QualifiedName);

    Optional<PaintServer> svg_paint_computed_value_to_paint_server(SVGPaintContext const& paint_context, Optional<CSS::SVGPaint> const& paint_value, double device_pixels_per_css_pixel) const;

    GC::Ptr<DOM::Element> resolve_url_to_element(CSS::URL const& url) const;
    GC::Ptr<DOM::Element> resolve_url_to_element(Utf16String const& url) const;

    template<typename T>
    GC::Ptr<T> try_resolve_url_to(CSS::URL const& url) const
    {
        return as_if<T>(resolve_url_to_element(url).ptr());
    }

    template<typename T>
    GC::Ptr<T> try_resolve_url_to(Utf16String const& url) const
    {
        return as_if<T>(resolve_url_to_element(url).ptr());
    }

private:
    virtual bool is_svg_graphics_element() const final { return true; }
    GC::Ptr<DOM::Element> resolve_fragment_identifier_to_element(Utf16String const& fragment) const;
    float resolve_relative_to_viewport_size(CSS::LengthPercentage const& length_percentage) const;

public:
    CSSPixels viewport_percentage_basis() const;
};

Gfx::AffineTransform transform_from_transform_list(ReadonlySpan<Transform> transform_list);

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGGraphicsElement>() const { return is_svg_graphics_element(); }

}

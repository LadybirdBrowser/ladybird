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
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedTransformList.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

struct SVGBoundingBoxOptions {
    bool fill { true };
    bool stroke { false };
    bool markers { false };
    bool clipped { false };
};

class WEB_API SVGGraphicsElement : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGGraphicsElement, SVGElement);

public:
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

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

    Optional<Painting::PaintStyle> fill_paint_style(SVGPaintContext const&, DisplayListRecordingContext* = nullptr) const;
    Optional<Painting::PaintStyle> stroke_paint_style(SVGPaintContext const&, DisplayListRecordingContext* = nullptr) const;

    GC::Ptr<SVG::SVGMaskElement const> mask() const;
    GC::Ptr<SVG::SVGClipPathElement const> clip_path() const;

    GC::Ptr<SVG::SVGPatternElement const> fill_pattern() const;
    GC::Ptr<SVG::SVGPatternElement const> stroke_pattern() const;

    WebIDL::ExceptionOr<GC::Ref<Geometry::DOMRect>> get_b_box(Optional<SVGBoundingBoxOptions>);
    GC::Ref<SVGAnimatedTransformList> transform() const;

    GC::Ptr<Geometry::DOMMatrix> get_ctm();
    GC::Ptr<Geometry::DOMMatrix> get_screen_ctm();

    virtual Gfx::AffineTransform element_transform() const
    {
        return m_transform;
    }

protected:
    SVGGraphicsElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Optional<Painting::PaintStyle> svg_paint_computed_value_to_gfx_paint_style(SVGPaintContext const& paint_context, Optional<CSS::SVGPaint> const& paint_value, DisplayListRecordingContext* = nullptr) const;

    Gfx::AffineTransform m_transform = {};

    GC::Ptr<DOM::Element> resolve_url_to_element(CSS::URL const& url) const;

    template<typename T>
    GC::Ptr<T> try_resolve_url_to(CSS::URL const& url) const
    {
        return as_if<T>(resolve_url_to_element(url).ptr());
    }

private:
    virtual bool is_svg_graphics_element() const final { return true; }
    float resolve_relative_to_viewport_size(CSS::LengthPercentage const& length_percentage) const;
};

Gfx::AffineTransform transform_from_transform_list(ReadonlySpan<Transform> transform_list);

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGGraphicsElement>() const { return is_svg_graphics_element(); }

}

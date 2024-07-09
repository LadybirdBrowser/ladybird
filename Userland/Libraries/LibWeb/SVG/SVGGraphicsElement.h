/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/PaintStyle.h>
#include <LibGfx/Path.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedTransformList.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/SVG/ViewBox.h>

namespace Web::SVG {

struct SVGBoundingBoxOptions {
    bool fill { true };
    bool stroke { false };
    bool markers { false };
    bool clipped { false };
};

class SVGGraphicsElement : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGGraphicsElement, SVGElement);

public:
    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    Optional<Gfx::Color> fill_color() const;
    Optional<Gfx::Color> stroke_color() const;
    Optional<float> stroke_width() const;
    Optional<float> fill_opacity() const;
    Optional<float> stroke_opacity() const;
    Optional<FillRule> fill_rule() const;
    Optional<ClipRule> clip_rule() const;

    float visible_stroke_width() const
    {
        if (auto color = stroke_color(); color.has_value() && color->alpha() > 0)
            return stroke_width().value_or(0);
        return 0;
    }

    Gfx::AffineTransform get_transform() const;

    Optional<Painting::PaintStyle> fill_paint_style(SVGPaintContext const&) const;
    Optional<Painting::PaintStyle> stroke_paint_style(SVGPaintContext const&) const;

    JS::GCPtr<SVG::SVGMaskElement const> mask() const;
    JS::GCPtr<SVG::SVGClipPathElement const> clip_path() const;

    JS::NonnullGCPtr<Geometry::DOMRect> get_b_box(Optional<SVGBoundingBoxOptions>);
    JS::NonnullGCPtr<SVGAnimatedTransformList> transform() const;

protected:
    SVGGraphicsElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual Gfx::AffineTransform element_transform() const
    {
        return m_transform;
    }

    Optional<Painting::PaintStyle> svg_paint_computed_value_to_gfx_paint_style(SVGPaintContext const& paint_context, Optional<CSS::SVGPaint> const& paint_value) const;

    Gfx::AffineTransform m_transform = {};

    template<typename T>
    JS::GCPtr<T> try_resolve_url_to(URL::URL const& url) const
    {
        if (!url.fragment().has_value())
            return {};
        auto node = document().get_element_by_id(*url.fragment());
        if (!node)
            return {};
        if (is<T>(*node))
            return static_cast<T&>(*node);
        return {};
    }

private:
    virtual bool is_svg_graphics_element() const final { return true; }
};

Gfx::AffineTransform transform_from_transform_list(ReadonlySpan<Transform> transform_list);

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGGraphicsElement>() const { return is_svg_graphics_element(); }

}

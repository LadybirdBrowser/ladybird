/*
 * Copyright (c) 2025, Ramon van Sprundel <ramonvansprundel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IterationDecision.h>
#include <LibGfx/PaintStyle.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGAnimatedTransformList.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGURIReference.h>
#include <LibWeb/SVG/SVGViewport.h>

namespace Web::SVG {

struct SVGPaintContext;

// https://www.w3.org/TR/SVG2/pservers.html#PatternElement
using PatternUnits = GradientUnits;

class SVGPatternElement
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes>
    , public SVGViewport {
    WEB_PLATFORM_OBJECT(SVGPatternElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGPatternElement);

public:
    virtual ~SVGPatternElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    GC::Ref<SVGAnimatedLength> x() const;
    GC::Ref<SVGAnimatedLength> y() const;
    GC::Ref<SVGAnimatedLength> width() const;
    GC::Ref<SVGAnimatedLength> height() const;

    // FIXME: This should return the right paint style
    Optional<Painting::PaintStyle> to_gfx_paint_style(SVGPaintContext const& context) const;

    GC::Ref<SVGAnimatedEnumeration> pattern_units() const;
    GC::Ref<SVGAnimatedEnumeration> pattern_content_units() const;
    GC::Ref<SVGAnimatedTransformList> pattern_transform() const;

    virtual Optional<ViewBox> view_box() const override { return m_view_box; }
    virtual Optional<PreserveAspectRatio> preserve_aspect_ratio() const override { return m_preserve_aspect_ratio; }

    GC::Ref<SVGAnimatedRect> view_box_for_bindings() { return *m_view_box_for_bindings; }

protected:
    SVGPatternElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    RefPtr<Gfx::Bitmap> create_pattern_bitmap(SVGPaintContext const&) const;
    GC::Ptr<SVGPatternElement const> linked_pattern(HashTable<SVGPatternElement const*>& seen_patterns) const;

    // TODO:
    // - href/xlink:href references
    // - Rendering child elements

    mutable RefPtr<Painting::SVGPatternPaintStyle> m_paint_style;
    Optional<ViewBox> m_view_box;
    Optional<PreserveAspectRatio> m_preserve_aspect_ratio;
    GC::Ptr<SVGAnimatedRect> m_view_box_for_bindings;

    mutable GC::Ptr<SVGAnimatedEnumeration> m_pattern_units;
    mutable GC::Ptr<SVGAnimatedEnumeration> m_pattern_content_units;
    mutable GC::Ptr<SVGAnimatedTransformList> m_pattern_transform;

    mutable GC::Ptr<SVGAnimatedLength> m_x;
    mutable GC::Ptr<SVGAnimatedLength> m_y;
    mutable GC::Ptr<SVGAnimatedLength> m_width;
    mutable GC::Ptr<SVGAnimatedLength> m_height;
};

}

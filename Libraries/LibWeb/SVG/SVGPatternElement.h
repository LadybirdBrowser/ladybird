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
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

struct SVGPaintContext;

// https://www.w3.org/TR/SVG2/pservers.html#PatternElement
using PatternUnits = GradientUnits;

class SVGPatternElement
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_PLATFORM_OBJECT(SVGPatternElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGPatternElement);

public:
    virtual ~SVGPatternElement() override = default;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    GC::Ref<SVGAnimatedLength> x();
    GC::Ref<SVGAnimatedLength> y();
    GC::Ref<SVGAnimatedLength> width();
    GC::Ref<SVGAnimatedLength> height();

    // FIXME: This should return the right paint style
    Optional<Painting::PaintStyle> to_gfx_paint_style(SVGPaintContext const& context) const;

    PatternUnits pattern_units() const;
    PatternUnits pattern_content_units() const;

protected:
    SVGPatternElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    RefPtr<Gfx::Bitmap> create_pattern_bitmap(SVGPaintContext const&) const;
    GC::Ptr<SVGPatternElement const> linked_pattern(HashTable<SVGPatternElement const*>& seen_patterns) const;

    // TODO:
    // - patternUnits and patternContentUnits attrs
    // - x, y, width, height attrs
    // - patternTransform
    // - viewBox and preserveAspectRatio
    // - href/xlink:href references
    // - Rendering child elements

    mutable RefPtr<Painting::SVGPatternPaintStyle> m_paint_style;
};
}

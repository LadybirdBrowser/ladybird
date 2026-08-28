/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Filter.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

// https://drafts.fxtf.org/filter-effects/#elementdef-filter
class SVGFilterElement final
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::No> {
    WEB_WRAPPABLE(SVGFilterElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFilterElement);

public:
    virtual ~SVGFilterElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    Optional<Gfx::Filter> gfx_filter(Layout::NodeWithStyle const& referenced_node, Gfx::FloatPoint filter_scale);

    GC::Ref<SVGAnimatedEnumeration> filter_units() const;
    GC::Ref<SVGAnimatedEnumeration> primitive_units() const;

    // https://drafts.csswg.org/filter-effects-1#dom-svgfilterelement-x
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x, Horizontal, SVGLengthValue::percentage(-10));

    // https://drafts.csswg.org/filter-effects-1#dom-svgfilterelement-y
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y, Vertical, SVGLengthValue::percentage(-10));

    // https://drafts.csswg.org/filter-effects-1#dom-svgfilterelement-width
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(width, Horizontal, SVGLengthValue::percentage(120));

    // https://drafts.csswg.org/filter-effects-1#dom-svgfilterelement-height
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(height, Vertical, SVGLengthValue::percentage(120));

private:
    SVGFilterElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Cell::Visitor&) override;

    Optional<SVGUnits> m_filter_units {};
    Optional<SVGUnits> m_primitive_units {};
};

}

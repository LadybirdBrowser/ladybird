/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Matrix4x4.h>
#include <LibGfx/PaintStyle.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/SVG/AttributeParsing.h>
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
    virtual Optional<ViewBox> active_view_box() const
    {
        if (auto const* fit_to_view_box = this->fit_to_view_box())
            return fit_to_view_box->view_box();
        return {};
    }

    WebIDL::ExceptionOr<GC::Ref<Geometry::DOMRect>> get_b_box(Bindings::SVGBoundingBoxOptions const&);
    GC::Ref<SVGAnimatedTransformList> transform() const;

    GC::Ptr<Geometry::DOMMatrix> get_ctm();
    GC::Ptr<Geometry::DOMMatrix> get_screen_ctm();

    GC::Ptr<DOM::Element> resolve_url_to_element(CSS::URL const& url) const;

protected:
    SVGGraphicsElement(DOM::Document&, DOM::QualifiedName);

private:
    virtual bool is_svg_graphics_element() const final { return true; }
    GC::Ptr<DOM::Element> resolve_fragment_identifier_to_element(Utf16String const& fragment) const;
};

Gfx::AffineTransform transform_from_transform_list(ReadonlySpan<Transform> transform_list);

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGGraphicsElement>() const { return is_svg_graphics_element(); }

}

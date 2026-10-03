/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/Path.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

class SVGPathElement final : public SVGGeometryElement {
    WEB_WRAPPABLE(SVGPathElement, SVGGeometryElement);
    GC_DECLARE_ALLOCATOR(SVGPathElement);

public:
    virtual ~SVGPathElement() override = default;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size, CSS::ComputedValues const&) override;

private:
    virtual bool is_svg_path_element() const final { return true; }

    SVGPathElement(DOM::Document&, DOM::QualifiedName);
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGPathElement>() const { return is_svg_path_element(); }

}

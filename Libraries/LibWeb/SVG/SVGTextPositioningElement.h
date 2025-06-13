/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGTextContentElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/text.html#InterfaceSVGTextPositioningElement
class SVGTextPositioningElement : public SVGTextContentElement {
    WEB_PLATFORM_OBJECT(SVGTextPositioningElement, SVGTextContentElement);

public:
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    Gfx::FloatPoint get_offset(CSSPixelSize const& viewport_size) const;

    GC::Ref<SVGAnimatedLength> x() const;
    GC::Ref<SVGAnimatedLength> y() const;
    GC::Ref<SVGAnimatedLength> dx() const;
    GC::Ref<SVGAnimatedLength> dy() const;

protected:
    SVGTextPositioningElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    Optional<NumberPercentage> m_x;
    Optional<NumberPercentage> m_y;
    Optional<NumberPercentage> m_dx;
    Optional<NumberPercentage> m_dy;
};

}

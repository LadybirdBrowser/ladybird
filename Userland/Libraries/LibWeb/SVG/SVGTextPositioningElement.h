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
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    Gfx::FloatPoint get_offset() const;

    JS::NonnullGCPtr<SVGAnimatedLength> x() const;
    JS::NonnullGCPtr<SVGAnimatedLength> y() const;
    JS::NonnullGCPtr<SVGAnimatedLength> dx() const;
    JS::NonnullGCPtr<SVGAnimatedLength> dy() const;

protected:
    SVGTextPositioningElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    float m_x { 0 };
    float m_y { 0 };
    float m_dx { 0 };
    float m_dy { 0 };
};

}

/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGradientElement.h>

namespace Web::SVG {

class SVGLinearGradientElement : public SVGGradientElement {
    WEB_WRAPPABLE(SVGLinearGradientElement, SVGGradientElement);
    GC_DECLARE_ALLOCATOR(SVGLinearGradientElement);

public:
    virtual ~SVGLinearGradientElement() override = default;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGLinearGradientElement__x1
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x1, Horizontal, SVGLengthValue::percentage(0));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGLinearGradientElement__y1
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y1, Vertical, SVGLengthValue::percentage(0));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGLinearGradientElement__x2
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(x2, Horizontal, SVGLengthValue::percentage(100));

    // https://w3c.github.io/svgwg/svg2-draft/pservers.html#__svg__SVGLinearGradientElement__y2
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE(y2, Vertical, SVGLengthValue::percentage(0));

protected:
    SVGLinearGradientElement(DOM::Document&, DOM::QualifiedName);

private:
    virtual bool is_radial_gradient() const override { return false; }
    virtual void collect_gradient_attributes(GradientAttributes&) const override;

    Optional<NumberPercentage> m_x1;
    Optional<NumberPercentage> m_y1;
    Optional<NumberPercentage> m_x2;
    Optional<NumberPercentage> m_y2;
};

}

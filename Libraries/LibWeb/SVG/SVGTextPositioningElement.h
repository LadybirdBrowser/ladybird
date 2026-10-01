/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGLengthValue.h>
#include <LibWeb/SVG/SVGTextContentElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/text.html#InterfaceSVGTextPositioningElement
class SVGTextPositioningElement : public SVGTextContentElement {
    WEB_WRAPPABLE(SVGTextPositioningElement, SVGTextContentElement);

public:
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    // The positioning attributes as written. A relative length follows the element's font rather than its attributes,
    // so the layout stage, not this parse, is what turns one into pixels.
    struct ParsedTextPositioning {
        Optional<SVGLengthValue> x;
        Optional<SVGLengthValue> y;
        Optional<SVGLengthValue> dx;
        Optional<SVGLengthValue> dy;
    };
    ParsedTextPositioning parsed_text_positioning() const;

    GC::Ref<SVGAnimatedLengthList> x();
    GC::Ref<SVGAnimatedLengthList> y();
    GC::Ref<SVGAnimatedLengthList> dx();
    GC::Ref<SVGAnimatedLengthList> dy();
    GC::Ref<SVGAnimatedNumberList> rotate();

protected:
    SVGTextPositioningElement(DOM::Document&, DOM::QualifiedName);
    virtual void visit_edges(Visitor&) override;

private:
    GC::Ref<SVGAnimatedLengthList> ensure_length_list(GC::Ptr<SVGAnimatedLengthList>&, Utf16FlyString const& attribute_name) const;

    GC::Ptr<SVGAnimatedLengthList> m_x;
    GC::Ptr<SVGAnimatedLengthList> m_y;
    GC::Ptr<SVGAnimatedLengthList> m_dx;
    GC::Ptr<SVGAnimatedLengthList> m_dy;
    GC::Ptr<SVGAnimatedNumberList> m_rotate;
};

}

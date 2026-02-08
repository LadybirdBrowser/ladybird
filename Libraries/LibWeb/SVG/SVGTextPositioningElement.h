/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/SVG/SVGTextContentElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/text.html#TSpanNotes
// https://svgwg.org/svg2-draft/text.html#TSpanAttributes
struct TextPositioning {
    using Position = Variant<CSS::LengthPercentage, CSS::Number>;

    Vector<Position> x;
    Vector<Position> y;
    Vector<Position> dx;
    Vector<Position> dy;
    Vector<float> rotate;

    void apply_to_text_position(Layout::Node const& node, CSSPixelSize viewport, Gfx::FloatPoint& current_text_position,
        size_t character_index) const
    {
        auto value_for_character = [&](Vector<Position> const& values) -> float {
            if (values.is_empty())
                return 0.f;

            auto position = character_index < values.size() ? values[character_index] : values.last();
            return position.visit(
                [](CSS::Number const& number) { return static_cast<float>(number.value()); },
                [&](CSS::LengthPercentage const& length_percentage) {
                    auto reference = &values == &x || &values == &dx ? viewport.width() : viewport.height();
                    return length_percentage.to_px(node, reference).to_float();
                });
        };

        if (!x.is_empty())
            current_text_position.set_x(value_for_character(x));
        if (!y.is_empty())
            current_text_position.set_y(value_for_character(y));

        current_text_position.translate_by(value_for_character(dx), value_for_character(dy));
    }
};

// https://svgwg.org/svg2-draft/text.html#InterfaceSVGTextPositioningElement
class SVGTextPositioningElement : public SVGTextContentElement {
    WEB_PLATFORM_OBJECT(SVGTextPositioningElement, SVGTextContentElement);

public:
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    TextPositioning text_positioning() const;

    GC::Ref<SVGAnimatedLengthList> x();
    GC::Ref<SVGAnimatedLengthList> y();
    GC::Ref<SVGAnimatedLengthList> dx();
    GC::Ref<SVGAnimatedLengthList> dy();
    GC::Ref<SVGAnimatedNumberList> rotate();

protected:
    SVGTextPositioningElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

private:
    GC::Ref<SVGAnimatedLengthList> ensure_length_list(GC::Ptr<SVGAnimatedLengthList>&, FlyString const& attribute_name) const;

    GC::Ptr<SVGAnimatedLengthList> m_x;
    GC::Ptr<SVGAnimatedLengthList> m_y;
    GC::Ptr<SVGAnimatedLengthList> m_dx;
    GC::Ptr<SVGAnimatedLengthList> m_dy;
    GC::Ptr<SVGAnimatedNumberList> m_rotate;
};

}

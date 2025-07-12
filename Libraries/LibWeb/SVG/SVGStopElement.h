/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/pservers.html#GradientStops
class SVGStopElement final : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGStopElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGStopElement);

public:
    virtual ~SVGStopElement() override = default;

    GC::Ref<SVGAnimatedNumber> offset();

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;

    float stop_offset() { return offset()->base_val(); }
    Gfx::Color stop_color() const;
    float stop_opacity() const;

private:
    SVGStopElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ptr<SVGAnimatedNumber> m_stop_offset;
};

}

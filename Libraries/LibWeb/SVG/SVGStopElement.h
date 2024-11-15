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

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    GC::Ref<SVGAnimatedNumber> offset() const;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;

    NumberPercentage stop_offset() const { return m_offset.value_or(NumberPercentage::create_number(0)); }
    Gfx::Color stop_color() const;
    float stop_opacity() const;

private:
    SVGStopElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    Optional<NumberPercentage> m_offset;
};

}

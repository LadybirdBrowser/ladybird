/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGFEDisplacementMapElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEDisplacementMapElement> {
    WEB_PLATFORM_OBJECT(SVGFEDisplacementMapElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEDisplacementMapElement);

public:
    enum class ChannelSelector {
        Unknown = 0,
        Red = 1,
        Green = 2,
        Blue = 3,
        Alpha = 4,
    };

    virtual ~SVGFEDisplacementMapElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedString> in2();
    GC::Ref<SVGAnimatedNumber> scale();
    GC::Ref<SVGAnimatedEnumeration> x_channel_selector() const;
    GC::Ref<SVGAnimatedEnumeration> y_channel_selector() const;

private:
    SVGFEDisplacementMapElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedString> m_in2;
    GC::Ptr<SVGAnimatedNumber> m_scale;
    GC::Ptr<SVGAnimatedEnumeration> m_x_channel_selector;
    GC::Ptr<SVGAnimatedEnumeration> m_y_channel_selector;
};

}

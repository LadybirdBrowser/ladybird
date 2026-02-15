/*
 * Copyright (c) 2026, Samuele Cerea <samu@cerea.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedLength.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGAnimatedString.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGFEDisplacementMapElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEDisplacementMapElement> {
    WEB_PLATFORM_OBJECT(SVGFEDisplacementMapElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEDisplacementMapElement);

public:
    virtual ~SVGFEDisplacementMapElement() override = default;

    GC::Ref<SVGAnimatedString> in1();
    GC::Ref<SVGAnimatedString> in2();

    GC::Ref<SVGAnimatedNumber> scale();

    enum class ChannelSelector : u8 {
        Unknown = 0,
        R = 1,
        G = 2,
        B = 3,
        A = 4
    };

    ChannelSelector x_channel_selector() const;
    ChannelSelector y_channel_selector() const;
    GC::Ref<SVGAnimatedEnumeration> x_channel_selector_bindings() const;
    GC::Ref<SVGAnimatedEnumeration> y_channel_selector_bindings() const;

private:
    SVGFEDisplacementMapElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& new_value, Optional<FlyString> const& namespace_) override;

    GC::Ptr<SVGAnimatedString> m_in1;
    GC::Ptr<SVGAnimatedString> m_in2;

    GC::Ptr<SVGAnimatedNumber> m_scale;
    Optional<ChannelSelector> m_x_channel_selector;
    Optional<ChannelSelector> m_y_channel_selector;
};

}

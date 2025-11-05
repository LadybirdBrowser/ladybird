/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/SVG/SVGNumberList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedNumber
class SVGAnimatedNumberList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedNumberList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGAnimatedNumberList);

public:
    [[nodiscard]] static GC::Ref<SVGAnimatedNumberList> create(JS::Realm&, GC::Ref<SVGNumberList>);
    virtual ~SVGAnimatedNumberList() override = default;

    // https://www.w3.org/TR/SVG2/types.html#__svg__SVGAnimatedLengthList__baseVal
    GC::Ref<SVGNumberList> base_val() const { return m_base_val; }

    // https://www.w3.org/TR/SVG2/types.html#__svg__SVGAnimatedLengthList__animVal
    GC::Ref<SVGNumberList> anim_val() const { return m_base_val; }

private:
    SVGAnimatedNumberList(JS::Realm&, GC::Ref<SVGNumberList>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<SVGNumberList> m_base_val;
};

}

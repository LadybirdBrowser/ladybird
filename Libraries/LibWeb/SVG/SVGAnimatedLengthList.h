/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/SVG/SVGLengthList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedLengthList
class SVGAnimatedLengthList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedLengthList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGAnimatedLengthList);

public:
    [[nodiscard]] static GC::Ref<SVGAnimatedLengthList> create(JS::Realm&, GC::Ref<SVGLengthList>);
    virtual ~SVGAnimatedLengthList() override = default;

    // https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedLengthList__baseVal
    GC::Ref<SVGLengthList> base_val() const { return m_base_val; }

    // https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedLengthList__animVal
    GC::Ref<SVGLengthList> anim_val() const { return m_base_val; }

private:
    SVGAnimatedLengthList(JS::Realm&, GC::Ref<SVGLengthList>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<SVGLengthList> m_base_val;
};

}

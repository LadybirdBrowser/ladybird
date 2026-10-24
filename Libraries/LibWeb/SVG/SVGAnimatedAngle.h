/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGAngle.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedAngle
class SVGAnimatedAngle final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGAnimatedAngle, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGAnimatedAngle);

public:
    [[nodiscard]] static GC::Ref<SVGAnimatedAngle> create(GC::Ref<SVGAngle> base_val, GC::Ref<SVGAngle> anim_val);
    virtual ~SVGAnimatedAngle() override;

    GC::Ref<SVGAngle> base_val() const { return m_base_val; }
    GC::Ref<SVGAngle> anim_val() const { return m_anim_val; }

private:
    SVGAnimatedAngle(GC::Ref<SVGAngle> base_val, GC::Ref<SVGAngle> anim_val);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<SVGAngle> m_base_val;
    GC::Ref<SVGAngle> m_anim_val;
};

}

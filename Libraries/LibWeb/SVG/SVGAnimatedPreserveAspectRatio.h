/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGPreserveAspectRatio.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/coords.html#InterfaceSVGAnimatedPreserveAspectRatio
class SVGAnimatedPreserveAspectRatio final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGAnimatedPreserveAspectRatio, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGAnimatedPreserveAspectRatio);

public:
    [[nodiscard]] static GC::Ref<SVGAnimatedPreserveAspectRatio> create(GC::Ref<SVGElement>);
    virtual ~SVGAnimatedPreserveAspectRatio() override;

    GC::Ref<SVGPreserveAspectRatio> base_val() const { return m_base_val; }
    GC::Ref<SVGPreserveAspectRatio> anim_val() const { return m_anim_val; }

private:
    SVGAnimatedPreserveAspectRatio(GC::Ref<SVGPreserveAspectRatio> base_val, GC::Ref<SVGPreserveAspectRatio> anim_val);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<SVGPreserveAspectRatio> m_base_val;
    GC::Ref<SVGPreserveAspectRatio> m_anim_val;
};

}

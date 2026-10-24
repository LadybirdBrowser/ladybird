/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGLengthList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedLengthList
class SVGAnimatedLengthList final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGAnimatedLengthList, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGAnimatedLengthList);

public:
    static constexpr size_t base_val_offset() { return offsetof(SVGAnimatedLengthList, m_base_val); }
    static constexpr size_t anim_val_offset() { return offsetof(SVGAnimatedLengthList, m_anim_val); }
    [[nodiscard]] static GC::Ref<SVGAnimatedLengthList> create(GC::Ref<SVGElement>, Utf16FlyString reflected_attribute, SVGLength::Directionality);
    virtual ~SVGAnimatedLengthList() override = default;

    // https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedLengthList__baseVal
    GC::Ref<SVGLengthList> base_val() const { return m_base_val; }

    // https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedLengthList__animVal
    GC::Ref<SVGLengthList> anim_val() const { return m_anim_val; }

    // https://svgwg.org/svg2-draft/types.html#SynchronizingReflectedValues
    void synchronize();

private:
    SVGAnimatedLengthList(GC::Ref<SVGLengthList> base_val, GC::Ref<SVGLengthList> anim_val);

    virtual void visit_edges(Visitor&) override;

    GC::Ref<SVGLengthList> m_base_val;
    GC::Ref<SVGLengthList> m_anim_val;
};

}

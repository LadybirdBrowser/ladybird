/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG11/types.html#InterfaceSVGAnimatedLength
class SVGAnimatedLength final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGAnimatedLength, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGAnimatedLength);

public:
    static constexpr size_t base_val_offset() { return offsetof(SVGAnimatedLength, m_base_val); }
    static constexpr size_t anim_val_offset() { return offsetof(SVGAnimatedLength, m_anim_val); }
    [[nodiscard]] static GC::Ref<SVGAnimatedLength> create(GC::Ref<SVGLength> base_val, GC::Ref<SVGLength> anim_val);
    virtual ~SVGAnimatedLength() override;

    GC::Ref<SVGLength> base_val() const { return m_base_val; }
    GC::Ref<SVGLength> anim_val() const { return m_anim_val; }

private:
    SVGAnimatedLength(GC::Ref<SVGLength> base_val, GC::Ref<SVGLength> anim_val);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<SVGLength> m_base_val;
    GC::Ref<SVGLength> m_anim_val;
};

}

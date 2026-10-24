/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedAngle.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedAngle);

GC::Ref<SVGAnimatedAngle> SVGAnimatedAngle::create(GC::Ref<SVGAngle> base_val, GC::Ref<SVGAngle> anim_val)
{
    return GC::Heap::the().allocate<SVGAnimatedAngle>(base_val, anim_val);
}

SVGAnimatedAngle::SVGAnimatedAngle(GC::Ref<SVGAngle> base_val, GC::Ref<SVGAngle> anim_val)
    : m_base_val(base_val)
    , m_anim_val(anim_val)
{
}

SVGAnimatedAngle::~SVGAnimatedAngle() = default;

void SVGAnimatedAngle::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

}

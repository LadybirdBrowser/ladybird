/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedRect);

GC::Ref<SVGAnimatedRect> SVGAnimatedRect::create()
{
    return GC::Heap::the().allocate<SVGAnimatedRect>();
}

SVGAnimatedRect::SVGAnimatedRect()
    : m_base_val(Geometry::DOMRect::create({ 0, 0, 0, 0 }))
    , m_anim_val(Geometry::DOMRect::create({ 0, 0, 0, 0 }))
{
}

SVGAnimatedRect::~SVGAnimatedRect() = default;

void SVGAnimatedRect::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

void SVGAnimatedRect::set_base_val(Gfx::DoubleRect const& rect)
{
    m_base_val->set(rect);
}

void SVGAnimatedRect::set_anim_val(Gfx::DoubleRect const& rect)
{
    m_anim_val->set(rect);
}

}

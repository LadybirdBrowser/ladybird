/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/DOMRectReadOnly.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMRect);

GC::Ref<DOMRect> DOMRect::create(double x, double y, double width, double height)
{
    return GC::Heap::the().allocate<DOMRect>(x, y, width, height);
}

GC::Ref<DOMRect> DOMRect::create(Gfx::FloatRect const& rect)
{
    return GC::Heap::the().allocate<DOMRect>(rect.x(), rect.y(), rect.width(), rect.height());
}

GC::Ref<DOMRect> DOMRect::create()
{
    return GC::Heap::the().allocate<DOMRect>();
}

GC::Ref<DOMRect> DOMRect::dom_rect_from_rect(Bindings::DOMRectInit const& other)
{
    return create(other.x, other.y, other.width, other.height);
}

DOMRect::DOMRect(double x, double y, double width, double height)
    : DOMRectReadOnly(x, y, width, height)
{
}

DOMRect::DOMRect()
    : DOMRectReadOnly()
{
}

DOMRect::~DOMRect() = default;

}

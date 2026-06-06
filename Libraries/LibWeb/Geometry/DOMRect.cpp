/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMRect);

WebIDL::ExceptionOr<GC::Ref<DOMRect>> DOMRect::construct_impl(double x, double y, double width, double height)
{
    return create(x, y, width, height);
}

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

// https://drafts.fxtf.org/geometry/#create-a-domrect-from-the-dictionary
GC::Ref<DOMRect> DOMRect::from_rect(JS::VM&, Bindings::DOMRectInit const& other)
{
    return from_rect(other);
}

GC::Ref<DOMRect> DOMRect::from_rect(Bindings::DOMRectInit const& other)
{
    return GC::Heap::the().allocate<DOMRect>(other.x, other.y, other.width, other.height);
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

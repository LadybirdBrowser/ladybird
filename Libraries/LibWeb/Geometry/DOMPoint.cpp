/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/DOMPointReadOnly.h>
#include <LibWeb/Geometry/DOMPoint.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMPoint);

GC::Ref<DOMPoint> DOMPoint::create(double x, double y, double z, double w)
{
    return GC::Heap::the().allocate<DOMPoint>(x, y, z, w);
}

GC::Ref<DOMPoint> DOMPoint::create()
{
    return GC::Heap::the().allocate<DOMPoint>();
}

GC::Ref<DOMPoint> DOMPoint::dom_point_from_point(Bindings::DOMPointInit const& other)
{
    return create(other.x, other.y, other.z, other.w);
}

DOMPoint::DOMPoint(double x, double y, double z, double w)
    : DOMPointReadOnly(x, y, z, w)
{
}

DOMPoint::DOMPoint()
    : DOMPointReadOnly()
{
}

DOMPoint::~DOMPoint() = default;

}

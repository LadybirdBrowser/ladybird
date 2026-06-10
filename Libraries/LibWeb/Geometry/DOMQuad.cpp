/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/DOMPointReadOnly.h>
#include <LibWeb/Bindings/DOMQuad.h>
#include <LibWeb/Bindings/DOMRectReadOnly.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Geometry/DOMQuad.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMQuad);

GC::Ref<DOMQuad> DOMQuad::create()
{
    return GC::Heap::the().allocate<DOMQuad>(DOMPoint::create(), DOMPoint::create(), DOMPoint::create(), DOMPoint::create());
}

GC::Ref<DOMQuad> DOMQuad::create(GC::Ref<DOMPoint> p1, GC::Ref<DOMPoint> p2, GC::Ref<DOMPoint> p3, GC::Ref<DOMPoint> p4)
{
    return GC::Heap::the().allocate<DOMQuad>(p1, p2, p3, p4);
}

static GC::Ref<DOMPoint> point_from_init(Bindings::DOMPointInit const& point)
{
    return DOMPoint::create(point.x, point.y, point.z, point.w);
}

static GC::Ref<DOMPoint> point_from_coordinates(double x, double y)
{
    return DOMPoint::create(x, y, 0, 1);
}

GC::Ref<DOMQuad> DOMQuad::create(Bindings::DOMPointInit const& p1, Bindings::DOMPointInit const& p2, Bindings::DOMPointInit const& p3, Bindings::DOMPointInit const& p4)
{
    return create(point_from_init(p1), point_from_init(p2), point_from_init(p3), point_from_init(p4));
}

GC::Ref<DOMQuad> DOMQuad::dom_quad_from_rect(Bindings::DOMRectInit const& other)
{
    return create(
        point_from_coordinates(other.x, other.y),
        point_from_coordinates(other.x + other.width, other.y),
        point_from_coordinates(other.x + other.width, other.y + other.height),
        point_from_coordinates(other.x, other.y + other.height));
}

GC::Ref<DOMQuad> DOMQuad::dom_quad_from_quad(Bindings::DOMQuadInit const& other)
{
    return create(
        point_from_init(other.p1.value_or(Bindings::DOMPointInit {})),
        point_from_init(other.p2.value_or(Bindings::DOMPointInit {})),
        point_from_init(other.p3.value_or(Bindings::DOMPointInit {})),
        point_from_init(other.p4.value_or(Bindings::DOMPointInit {})));
}

DOMQuad::DOMQuad(GC::Ref<DOMPoint> p1, GC::Ref<DOMPoint> p2, GC::Ref<DOMPoint> p3, GC::Ref<DOMPoint> p4)
    : m_p1(p1)
    , m_p2(p2)
    , m_p3(p3)
    , m_p4(p4)
{
}

DOMQuad::~DOMQuad() = default;

// https://drafts.fxtf.org/geometry/#dom-domquad-getbounds
GC::Ref<DOMRect> DOMQuad::get_bounds() const
{
    // The NaN-safe minimum of a non-empty list of unrestricted double values is NaN if any member of the list is NaN, or the minimum of the list otherwise.
    auto nan_safe_minimum = [](double a, double b, double c, double d) -> double {
        if (isnan(a) || isnan(b) || isnan(c) || isnan(d))
            return NAN;
        return min(a, min(b, min(c, d)));
    };

    // Analogously, the NaN-safe maximum of a non-empty list of unrestricted double values is NaN if any member of the list is NaN, or the maximum of the list otherwise.
    auto nan_safe_maximum = [](double a, double b, double c, double d) -> double {
        if (isnan(a) || isnan(b) || isnan(c) || isnan(d))
            return NAN;
        return max(a, max(b, max(c, d)));
    };

    // 1. Let bounds be a DOMRect object.
    auto bounds = DOMRect::create({});

    // 2. Let left be the NaN-safe minimum of point 1’s x coordinate, point 2’s x coordinate, point 3’s x coordinate and point 4’s x coordinate.
    auto left = nan_safe_minimum(m_p1->x(), m_p2->x(), m_p3->x(), m_p4->x());

    // 3. Let top be the NaN-safe minimum of point 1’s y coordinate, point 2’s y coordinate, point 3’s y coordinate and point 4’s y coordinate.
    auto top = nan_safe_minimum(m_p1->y(), m_p2->y(), m_p3->y(), m_p4->y());

    // 4. Let right be the NaN-safe maximum of point 1’s x coordinate, point 2’s x coordinate, point 3’s x coordinate and point 4’s x coordinate.
    auto right = nan_safe_maximum(m_p1->x(), m_p2->x(), m_p3->x(), m_p4->x());

    // 5. Let bottom be the NaN-safe maximum of point 1’s y coordinate, point 2’s y coordinate, point 3’s y coordinate and point 4’s y coordinate.
    auto bottom = nan_safe_maximum(m_p1->y(), m_p2->y(), m_p3->y(), m_p4->y());

    // 6. Set x coordinate of bounds to left, y coordinate of bounds to top, width dimension of bounds to right - left and height dimension of bounds to bottom - top.
    bounds->set_x(left);
    bounds->set_y(top);
    bounds->set_width(right - left);
    bounds->set_height(bottom - top);

    // 7. Return bounds.
    return bounds;
}

void DOMQuad::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_p1);
    visitor.visit(m_p2);
    visitor.visit(m_p3);
    visitor.visit(m_p4);
}

// https://drafts.fxtf.org/geometry/#structured-serialization
WebIDL::ExceptionOr<void> DOMQuad::serialization_steps(JS::Realm& realm, HTML::TransferDataEncoder& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    // 1. Set serialized.[[P1]] to the sub-serialization of value’s point 1.
    TRY(m_p1->serialization_steps(realm, serialized, for_storage, memory));

    // 2. Set serialized.[[P2]] to the sub-serialization of value’s point 2.
    TRY(m_p2->serialization_steps(realm, serialized, for_storage, memory));

    // 3. Set serialized.[[P3]] to the sub-serialization of value’s point 3.
    TRY(m_p3->serialization_steps(realm, serialized, for_storage, memory));

    // 4. Set serialized.[[P4]] to the sub-serialization of value’s point 4.
    TRY(m_p4->serialization_steps(realm, serialized, for_storage, memory));

    return {};
}

// https://drafts.fxtf.org/geometry/#structured-serialization
WebIDL::ExceptionOr<void> DOMQuad::deserialization_steps(JS::Realm& realm, HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory& memory)
{
    auto deserialize_dom_point = [&](GC::Ref<DOMPoint>& storage) -> WebIDL::ExceptionOr<void> {
        storage = DOMPoint::create();
        TRY(storage->deserialization_steps(realm, serialized, memory));
        return {};
    };

    // 1. Set value’s point 1 to the sub-deserialization of serialized.[[P1]].
    TRY(deserialize_dom_point(m_p1));

    // 2. Set value’s point 2 to the sub-deserialization of serialized.[[P2]].
    TRY(deserialize_dom_point(m_p2));

    // 3. Set value’s point 3 to the sub-deserialization of serialized.[[P3]].
    TRY(deserialize_dom_point(m_p3));

    // 4. Set value’s point 4 to the sub-deserialization of serialized.[[P4]].
    TRY(deserialize_dom_point(m_p4));

    return {};
}

}

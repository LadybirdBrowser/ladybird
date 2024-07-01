/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMQuadPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geometry/DOMQuad.h>

namespace Web::Geometry {

JS_DEFINE_ALLOCATOR(DOMQuad);

JS::NonnullGCPtr<DOMQuad> DOMQuad::construct_impl(JS::Realm& realm, DOMPointInit const& p1, DOMPointInit const& p2, DOMPointInit const& p3, DOMPointInit const& p4)
{
    return realm.heap().allocate<DOMQuad>(realm, realm, p1, p2, p3, p4);
}

JS::NonnullGCPtr<DOMQuad> DOMQuad::create(JS::Realm& realm)
{
    return realm.heap().allocate<DOMQuad>(realm, realm);
}

DOMQuad::DOMQuad(JS::Realm& realm, DOMPointInit const& p1, DOMPointInit const& p2, DOMPointInit const& p3, DOMPointInit const& p4)
    : PlatformObject(realm)
    , m_p1(DOMPoint::from_point(realm.vm(), p1))
    , m_p2(DOMPoint::from_point(realm.vm(), p2))
    , m_p3(DOMPoint::from_point(realm.vm(), p3))
    , m_p4(DOMPoint::from_point(realm.vm(), p4))
{
}

DOMQuad::DOMQuad(JS::Realm& realm)
    : PlatformObject(realm)
    , m_p1(DOMPoint::create(realm))
    , m_p2(DOMPoint::create(realm))
    , m_p3(DOMPoint::create(realm))
    , m_p4(DOMPoint::create(realm))
{
}

DOMQuad::~DOMQuad() = default;

// https://drafts.fxtf.org/geometry/#dom-domquad-fromrect
JS::NonnullGCPtr<DOMQuad> DOMQuad::from_rect(JS::VM& vm, DOMRectInit const& other)
{
    // The fromRect(other) static method on DOMQuad must create a DOMQuad from the DOMRectInit dictionary other.
    return construct_impl(*vm.current_realm(), { other.x, other.y },
        { other.x + other.width, other.y },
        { other.x + other.width, other.y + other.height },
        { other.x, other.y + other.height });
}

// https://drafts.fxtf.org/geometry/#dom-domquad-fromquad
JS::NonnullGCPtr<DOMQuad> DOMQuad::from_quad(JS::VM& vm, DOMQuadInit const& other)
{
    // The fromQuad(other) static method on DOMQuad must create a DOMQuad from the DOMQuadInit dictionary other.
    return construct_impl(*vm.current_realm(), other.p1, other.p2, other.p3, other.p4);
}

// https://drafts.fxtf.org/geometry/#dom-domquad-getbounds
JS::NonnullGCPtr<DOMRect> DOMQuad::get_bounds() const
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
    auto bounds = DOMRect::create(realm(), {});

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

// https://drafts.fxtf.org/geometry/#structured-serialization
WebIDL::ExceptionOr<void> DOMQuad::serialization_steps(HTML::SerializationRecord& serialzied, bool for_storage, HTML::SerializationMemory& memory)
{
    auto& vm = this->vm();
    // 1. Set serialized.[[P1]] to the sub-serialization of value’s point 1.
    serialzied.extend(TRY(HTML::structured_serialize_internal(vm, m_p1, for_storage, memory)));
    // 2. Set serialized.[[P2]] to the sub-serialization of value’s point 2.
    serialzied.extend(TRY(HTML::structured_serialize_internal(vm, m_p2, for_storage, memory)));
    // 3. Set serialized.[[P3]] to the sub-serialization of value’s point 3.
    serialzied.extend(TRY(HTML::structured_serialize_internal(vm, m_p3, for_storage, memory)));
    // 4. Set serialized.[[P4]] to the sub-serialization of value’s point 4.
    serialzied.extend(TRY(HTML::structured_serialize_internal(vm, m_p4, for_storage, memory)));

    return {};
}

// https://drafts.fxtf.org/geometry/#structured-serialization
WebIDL::ExceptionOr<void> DOMQuad::deserialization_steps(ReadonlySpan<u32> const& serialized, size_t& position, HTML::DeserializationMemory& memory)
{
    auto& realm = this->realm();
    // 1. Set value’s point 1 to the sub-deserialization of serialized.[[P1]].
    auto deserialized_record = TRY(HTML::structured_deserialize_internal(vm(), serialized, realm, memory, position));
    if (deserialized_record.value.has_value() && is<DOMPoint>(deserialized_record.value.value().as_object()))
        m_p1 = dynamic_cast<DOMPoint&>(deserialized_record.value.release_value().as_object());
    position = deserialized_record.position;

    // 2. Set value’s point 2 to the sub-deserialization of serialized.[[P2]].
    deserialized_record = TRY(HTML::structured_deserialize_internal(vm(), serialized, realm, memory, position));
    if (deserialized_record.value.has_value() && is<DOMPoint>(deserialized_record.value.value().as_object()))
        m_p2 = dynamic_cast<DOMPoint&>(deserialized_record.value.release_value().as_object());
    position = deserialized_record.position;

    // 3. Set value’s point 3 to the sub-deserialization of serialized.[[P3]].
    deserialized_record = TRY(HTML::structured_deserialize_internal(vm(), serialized, realm, memory, position));
    if (deserialized_record.value.has_value() && is<DOMPoint>(deserialized_record.value.value().as_object()))
        m_p3 = dynamic_cast<DOMPoint&>(deserialized_record.value.release_value().as_object());
    position = deserialized_record.position;

    // 4. Set value’s point 4 to the sub-deserialization of serialized.[[P4]].
    deserialized_record = TRY(HTML::structured_deserialize_internal(vm(), serialized, realm, memory, position));
    if (deserialized_record.value.has_value() && is<DOMPoint>(deserialized_record.value.value().as_object()))
        m_p4 = dynamic_cast<DOMPoint&>(deserialized_record.value.release_value().as_object());
    position = deserialized_record.position;

    return {};
}

void DOMQuad::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMQuad);
}

void DOMQuad::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_p1);
    visitor.visit(m_p2);
    visitor.visit(m_p3);
    visitor.visit(m_p4);
}

}

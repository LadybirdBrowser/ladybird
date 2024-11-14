/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMPointPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geometry/DOMPoint.h>

namespace Web::Geometry {

GC_DEFINE_ALLOCATOR(DOMPoint);

GC::Ref<DOMPoint> DOMPoint::construct_impl(JS::Realm& realm, double x, double y, double z, double w)
{
    return realm.create<DOMPoint>(realm, x, y, z, w);
}

GC::Ref<DOMPoint> DOMPoint::create(JS::Realm& realm)
{
    return realm.create<DOMPoint>(realm);
}

DOMPoint::DOMPoint(JS::Realm& realm, double x, double y, double z, double w)
    : DOMPointReadOnly(realm, x, y, z, w)
{
}

DOMPoint::DOMPoint(JS::Realm& realm)
    : DOMPointReadOnly(realm)
{
}

// https://drafts.fxtf.org/geometry/#dom-dompoint-frompoint
GC::Ref<DOMPoint> DOMPoint::from_point(JS::VM& vm, DOMPointInit const& other)
{
    // The fromPoint(other) static method on DOMPoint must create a DOMPoint from the dictionary other.
    return construct_impl(*vm.current_realm(), other.x, other.y, other.z, other.w);
}

DOMPoint::~DOMPoint() = default;

void DOMPoint::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMPoint);
}

}

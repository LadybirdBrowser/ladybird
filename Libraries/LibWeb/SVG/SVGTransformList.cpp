/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGTransformListPrototype.h>
#include <LibWeb/SVG/SVGTransformList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTransformList);

GC::Ref<SVGTransformList> SVGTransformList::create(JS::Realm& realm, Vector<GC::Ref<SVGTransform>> items, ReadOnlyList read_only)
{
    return realm.create<SVGTransformList>(realm, move(items), read_only);
}

GC::Ref<SVGTransformList> SVGTransformList::create(JS::Realm& realm, ReadOnlyList read_only)
{
    return realm.create<SVGTransformList>(realm, read_only);
}

SVGTransformList::SVGTransformList(JS::Realm& realm, Vector<GC::Ref<SVGTransform>> items, ReadOnlyList read_only)
    : Bindings::PlatformObject(realm)
    , SVGList(realm, move(items), read_only)
{
}

SVGTransformList::SVGTransformList(JS::Realm& realm, ReadOnlyList read_only)
    : Bindings::PlatformObject(realm)
    , SVGList(realm, read_only)
{
}

void SVGTransformList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTransformList);
    Base::initialize(realm);
}

void SVGTransformList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}

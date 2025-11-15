/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGNumberListPrototype.h>
#include <LibWeb/SVG/SVGNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGNumberList);

GC::Ref<SVGNumberList> SVGNumberList::create(JS::Realm& realm, Vector<GC::Ref<SVGNumber>> items, ReadOnlyList read_only)
{
    return realm.create<SVGNumberList>(realm, move(items), read_only);
}

GC::Ref<SVGNumberList> SVGNumberList::create(JS::Realm& realm, ReadOnlyList read_only)
{
    return realm.create<SVGNumberList>(realm, read_only);
}

SVGNumberList::SVGNumberList(JS::Realm& realm, Vector<GC::Ref<SVGNumber>> items, ReadOnlyList read_only)
    : Bindings::PlatformObject(realm)
    , SVGList(realm, move(items), read_only)
{
}

SVGNumberList::SVGNumberList(JS::Realm& realm, ReadOnlyList read_only)
    : Bindings::PlatformObject(realm)
    , SVGList(realm, read_only)
{
}

void SVGNumberList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGNumberList);
    Base::initialize(realm);
}

void SVGNumberList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}

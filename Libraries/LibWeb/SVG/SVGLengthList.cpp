/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGLengthListPrototype.h>
#include <LibWeb/SVG/SVGLengthList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLengthList);

GC::Ref<SVGLengthList> SVGLengthList::create(JS::Realm& realm, Vector<GC::Ref<SVGLength>> items, ReadOnlyList read_only)
{
    return realm.create<SVGLengthList>(realm, move(items), read_only);
}

GC::Ref<SVGLengthList> SVGLengthList::create(JS::Realm& realm, ReadOnlyList read_only)
{
    return realm.create<SVGLengthList>(realm, read_only);
}

SVGLengthList::SVGLengthList(JS::Realm& realm, Vector<GC::Ref<SVGLength>> items, ReadOnlyList read_only)
    : Bindings::PlatformObject(realm)
    , SVGList(realm, move(items), read_only)
{
}

SVGLengthList::SVGLengthList(JS::Realm& realm, ReadOnlyList read_only)
    : Bindings::PlatformObject(realm)
    , SVGList(realm, read_only)
{
}

void SVGLengthList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGLengthList);
    Base::initialize(realm);
}

void SVGLengthList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGList::visit_edges(visitor);
}

}

/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedNumberListPrototype.h>
#include <LibWeb/SVG/SVGAnimatedNumberList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedNumberList);

GC::Ref<SVGAnimatedNumberList> SVGAnimatedNumberList::create(JS::Realm& realm, GC::Ref<SVGNumberList> base_val)
{
    return realm.create<SVGAnimatedNumberList>(realm, base_val);
}

SVGAnimatedNumberList::SVGAnimatedNumberList(JS::Realm& realm, GC::Ref<SVGNumberList> base_val)
    : PlatformObject(realm)
    , m_base_val(base_val)
{
}

void SVGAnimatedNumberList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedNumberList);
    Base::initialize(realm);
}

void SVGAnimatedNumberList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
}

}

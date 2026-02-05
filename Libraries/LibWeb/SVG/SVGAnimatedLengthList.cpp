/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedLengthListPrototype.h>
#include <LibWeb/SVG/SVGAnimatedLengthList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedLengthList);

GC::Ref<SVGAnimatedLengthList> SVGAnimatedLengthList::create(JS::Realm& realm, GC::Ref<SVGLengthList> base_val)
{
    return realm.create<SVGAnimatedLengthList>(realm, base_val);
}

SVGAnimatedLengthList::SVGAnimatedLengthList(JS::Realm& realm, GC::Ref<SVGLengthList> base_val)
    : PlatformObject(realm)
    , m_base_val(base_val)
{
}

void SVGAnimatedLengthList::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedLengthList);
    Base::initialize(realm);
}

void SVGAnimatedLengthList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
}

}

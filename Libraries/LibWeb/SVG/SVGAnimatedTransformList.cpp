/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedTransformListPrototype.h>
#include <LibWeb/SVG/SVGAnimatedTransformList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedTransformList);

GC::Ref<SVGAnimatedTransformList> SVGAnimatedTransformList::create(JS::Realm& realm, GC::Ref<SVGTransformList> base_val, GC::Ref<SVGTransformList> anim_val)
{
    return realm.create<SVGAnimatedTransformList>(realm, base_val, anim_val);
}

SVGAnimatedTransformList::SVGAnimatedTransformList(JS::Realm& realm, GC::Ref<SVGTransformList> base_val, GC::Ref<SVGTransformList> anim_val)
    : PlatformObject(realm)
    , m_base_val(base_val)
    , m_anim_val(anim_val) {};

SVGAnimatedTransformList::~SVGAnimatedTransformList() = default;

void SVGAnimatedTransformList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedTransformList);
}

void SVGAnimatedTransformList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

}

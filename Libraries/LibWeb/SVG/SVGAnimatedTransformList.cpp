/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedTransformListPrototype.h>
#include <LibWeb/SVG/SVGAnimatedTransformList.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGAnimatedTransformList);

JS::NonnullGCPtr<SVGAnimatedTransformList> SVGAnimatedTransformList::create(JS::Realm& realm, JS::NonnullGCPtr<SVGTransformList> base_val, JS::NonnullGCPtr<SVGTransformList> anim_val)
{
    return realm.heap().allocate<SVGAnimatedTransformList>(realm, realm, base_val, anim_val);
}

SVGAnimatedTransformList::SVGAnimatedTransformList(JS::Realm& realm, JS::NonnullGCPtr<SVGTransformList> base_val, JS::NonnullGCPtr<SVGTransformList> anim_val)
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

/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SVGAnimatedLengthList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedLengthList);

GC::Ref<SVGAnimatedLengthList> SVGAnimatedLengthList::create(JS::Realm& realm, GC::Ref<SVGLengthList> base_val)
{
    return realm.create<SVGAnimatedLengthList>(realm, base_val);
}

SVGAnimatedLengthList::SVGAnimatedLengthList(JS::Realm& realm, GC::Ref<SVGLengthList> base_val)
    : Bindings::Wrappable(realm)
    , m_base_val(base_val)
{
}

void SVGAnimatedLengthList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
}

}

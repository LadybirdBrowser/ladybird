/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedLengthPrototype.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedLength);

GC::Ref<SVGAnimatedLength> SVGAnimatedLength::create(JS::Realm& realm, GC::Ref<SVGLength> base_val, GC::Ref<SVGLength> anim_val)
{
    return realm.create<SVGAnimatedLength>(realm, base_val, anim_val);
}

SVGAnimatedLength::SVGAnimatedLength(JS::Realm& realm, GC::Ref<SVGLength> base_val, GC::Ref<SVGLength> anim_val)
    : PlatformObject(realm)
    , m_base_val(base_val)
    , m_anim_val(anim_val)
{
    // The object referenced by animVal will always be distinct from the one referenced by baseVal, even when the
    // attribute is not animated.
    VERIFY(m_base_val.ptr() != m_anim_val.ptr());

    // https://svgwg.org/svg2-draft/types.html#InterfaceSVGLength
    // SVGLength objects reflected through the animVal IDL attribute are always read only.
    VERIFY(m_anim_val->read_only() == SVGLength::ReadOnly::Yes);
}

SVGAnimatedLength::~SVGAnimatedLength() = default;

void SVGAnimatedLength::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedLength);
    Base::initialize(realm);
}

void SVGAnimatedLength::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

}

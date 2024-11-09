/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedLengthPrototype.h>
#include <LibWeb/SVG/SVGAnimatedLength.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGAnimatedLength);

JS::NonnullGCPtr<SVGAnimatedLength> SVGAnimatedLength::create(JS::Realm& realm, JS::NonnullGCPtr<SVGLength> base_val, JS::NonnullGCPtr<SVGLength> anim_val)
{
    return realm.heap().allocate<SVGAnimatedLength>(realm, realm, move(base_val), move(anim_val));
}

SVGAnimatedLength::SVGAnimatedLength(JS::Realm& realm, JS::NonnullGCPtr<SVGLength> base_val, JS::NonnullGCPtr<SVGLength> anim_val)
    : PlatformObject(realm)
    , m_base_val(move(base_val))
    , m_anim_val(move(anim_val))
{
    // The object referenced by animVal will always be distinct from the one referenced by baseVal, even when the attribute is not animated.
    VERIFY(m_base_val.ptr() != m_anim_val.ptr());
}

SVGAnimatedLength::~SVGAnimatedLength() = default;

void SVGAnimatedLength::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedLength);
}

void SVGAnimatedLength::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_base_val);
    visitor.visit(m_anim_val);
}

}

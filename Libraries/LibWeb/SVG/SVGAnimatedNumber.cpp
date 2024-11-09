/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedNumberPrototype.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGAnimatedNumber);

JS::NonnullGCPtr<SVGAnimatedNumber> SVGAnimatedNumber::create(JS::Realm& realm, float base_val, float anim_val)
{
    return realm.heap().allocate<SVGAnimatedNumber>(realm, realm, base_val, anim_val);
}

SVGAnimatedNumber::SVGAnimatedNumber(JS::Realm& realm, float base_val, float anim_val)
    : PlatformObject(realm)
    , m_base_val(base_val)
    , m_anim_val(anim_val)
{
}

SVGAnimatedNumber::~SVGAnimatedNumber() = default;

void SVGAnimatedNumber::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedNumber);
}

}

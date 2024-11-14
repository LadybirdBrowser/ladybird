/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedNumberPrototype.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedNumber);

GC::Ref<SVGAnimatedNumber> SVGAnimatedNumber::create(JS::Realm& realm, float base_val, float anim_val)
{
    return realm.create<SVGAnimatedNumber>(realm, base_val, anim_val);
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

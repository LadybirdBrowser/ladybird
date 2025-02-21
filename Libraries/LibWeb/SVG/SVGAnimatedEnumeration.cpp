/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGAnimatedEnumerationPrototype.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAnimatedEnumeration);

GC::Ref<SVGAnimatedEnumeration> SVGAnimatedEnumeration::create(JS::Realm& realm, u16 value)
{
    return realm.create<SVGAnimatedEnumeration>(realm, value);
}

SVGAnimatedEnumeration::SVGAnimatedEnumeration(JS::Realm& realm, u16 value)
    : PlatformObject(realm)
    , m_value(value)
{
}

SVGAnimatedEnumeration::~SVGAnimatedEnumeration() = default;

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedEnumeration__baseVal
WebIDL::ExceptionOr<void> SVGAnimatedEnumeration::set_base_val(u16 base_val)
{
    // 1. Let value be the value being assigned to baseVal.
    auto value = base_val;

    // FIXME: 2. If value is 0 or is not the numeric type value for any value of the reflected attribute, then throw a
    //    TypeError.
    if (value == 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "invalid value for baseVal"sv };

    // FIXME: 3. Otherwise, if the reflecting IDL attribute is orientType and value is SVG_MARKER_ORIENT_ANGLE, then set the
    //    reflected attribute to the string "0".

    // FIXME: 4. Otherwise, value is the numeric type value for a specific, single keyword value for the reflected attribute.
    //    Set the reflected attribute to that value.
    m_value = value;

    return {};
}

void SVGAnimatedEnumeration::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAnimatedEnumeration);
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAnimatedEnumeration__baseVal
u16 SVGAnimatedEnumeration::base_or_anim_value() const
{
    // FIXME: 1. Let value be the value of the reflected attribute (using the attribute's initial value if it is not present or
    //    invalid).

    // FIXME: 2. Return the numeric type value for value, according to the reflecting IDL attribute's definition.

    return m_value;
}

}

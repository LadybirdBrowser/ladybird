/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFunctionDescriptors.h"
#include <LibWeb/Bindings/CSSFunctionDescriptors.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFunctionDescriptors);

GC::Ref<CSSFunctionDescriptors> CSSFunctionDescriptors::create(JS::Realm& realm, Vector<Descriptor> descriptors)
{
    return realm.create<CSSFunctionDescriptors>(realm, move(descriptors));
}

void CSSFunctionDescriptors::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSFunctionDescriptors);
    Base::initialize(realm);
}

// https://drafts.csswg.org/css-mixins-1/#dom-cssfunctiondescriptors-result
String CSSFunctionDescriptors::result() const
{
    return get_property_value("result"_string);
}

// https://drafts.csswg.org/css-mixins-1/#dom-cssfunctiondescriptors-result
WebIDL::ExceptionOr<void> CSSFunctionDescriptors::set_result(StringView value)
{
    return set_property("result"_string, value, ""sv);
}

}

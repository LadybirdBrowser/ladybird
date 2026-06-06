/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSFunctionDescriptors.h"
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/CSSFunctionDescriptors.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSFunctionDescriptors);

GC::Ref<CSSFunctionDescriptors> CSSFunctionDescriptors::create(Vector<Descriptor> descriptors)
{
    return GC::Heap::the().allocate<CSSFunctionDescriptors>(move(descriptors));
}

// https://drafts.csswg.org/css-mixins-1/#dom-cssfunctiondescriptors-result
String CSSFunctionDescriptors::result() const
{
    return get_property_value("result"_string);
}

// https://drafts.csswg.org/css-mixins-1/#dom-cssfunctiondescriptors-result
WebIDL::ExceptionOr<void> CSSFunctionDescriptors::set_result(JS::Realm& realm, StringView value)
{
    return set_property(realm, "result"_string, value, ""sv);
}

}

/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PropertyDescriptor.h>

namespace JS {

// 6.2.5.1 IsAccessorDescriptor ( Desc ), https://tc39.es/ecma262/#sec-isaccessordescriptor
bool PropertyDescriptor::is_accessor_descriptor() const
{
    // 1. If Desc is undefined, return false.

    // 2. If Desc has a [[Get]] field, return true.
    if (get.has_value())
        return true;

    // 3. If Desc has a [[Set]] field, return true.
    if (set.has_value())
        return true;

    // 4. Return false.
    return false;
}

// 6.2.5.2 IsDataDescriptor ( Desc ), https://tc39.es/ecma262/#sec-isdatadescriptor
bool PropertyDescriptor::is_data_descriptor() const
{
    // 1. If Desc is undefined, return false.

    // 2. If Desc has a [[Value]] field, return true.
    if (value.has_value())
        return true;

    // 3. If Desc has a [[Writable]] field, return true.
    if (writable.has_value())
        return true;

    // 4. Return false.
    return false;
}

// 6.2.5.3 IsGenericDescriptor ( Desc ), https://tc39.es/ecma262/#sec-isgenericdescriptor
bool PropertyDescriptor::is_generic_descriptor() const
{
    // 1. If Desc is undefined, return false.

    // 2. If IsAccessorDescriptor(Desc) is true, return false.
    if (is_accessor_descriptor())
        return false;

    // 3. If IsDataDescriptor(Desc) is true, return false.
    if (is_data_descriptor())
        return false;

    // 4. Return true.
    return true;
}

void PropertyDescriptor::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(value);
    visitor.visit(get);
    visitor.visit(set);
}

}

/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/kmalloc.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/PropertyAttributes.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// An element of an object's indexed storage, as Object::indexed_take_first() returns it.
struct ValueAndAttributes {
    Value value;
    PropertyAttributes attributes { default_attributes };

    Optional<u32> property_offset {};

    void visit_edges(Cell::Visitor& visitor)
    {
        visitor.visit(value);
    }
};

}

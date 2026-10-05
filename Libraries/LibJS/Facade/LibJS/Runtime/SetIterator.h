/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Set.h>

namespace JS {

class JS_API SetIterator final : public Object {
public:
    // The iteration kind is an Object::PropertyKind: values, or [value, value] arrays for keys and values.
    template<Enum PropertyKind>
    static GC::Ref<SetIterator> create(Realm& realm, Set& set, PropertyKind iteration_kind)
    {
        return create_of_property_kind(realm, set, to_underlying(iteration_kind));
    }

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_SET_ITERATOR; }

private:
    static GC::Ref<SetIterator> create_of_property_kind(Realm&, Set&, u8 property_kind);
};

}

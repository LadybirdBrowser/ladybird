/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API MapIterator final : public Object {
public:
    // The iteration kind is an Object::PropertyKind: keys, values, or [key, value] arrays.
    template<Enum PropertyKind>
    static GC::Ref<MapIterator> create(Realm& realm, Map& map, PropertyKind iteration_kind)
    {
        return create_of_property_kind(realm, map, to_underlying(iteration_kind));
    }

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_MAP_ITERATOR; }

private:
    static GC::Ref<MapIterator> create_of_property_kind(Realm&, Map&, u8 property_kind);
};

}

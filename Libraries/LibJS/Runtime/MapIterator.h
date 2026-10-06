/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API MapIterator final : public Object {
public:
    static GC::Ref<MapIterator> create(Realm&, Map& map, Object::PropertyKind iteration_kind);

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_MAP_ITERATOR; }
};

}

/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Set.h>

namespace JS {

class JS_API SetIterator final : public Object {
public:
    static GC::Ref<SetIterator> create(Realm&, Set& set, Object::PropertyKind iteration_kind);

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_SET_ITERATOR; }
};

}

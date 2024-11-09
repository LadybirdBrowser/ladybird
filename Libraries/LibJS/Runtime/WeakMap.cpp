/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/WeakMap.h>

namespace JS {

JS_DEFINE_ALLOCATOR(WeakMap);

NonnullGCPtr<WeakMap> WeakMap::create(Realm& realm)
{
    return realm.heap().allocate<WeakMap>(realm, realm.intrinsics().weak_map_prototype());
}

WeakMap::WeakMap(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , WeakContainer(heap())
{
}

void WeakMap::remove_dead_cells(Badge<Heap>)
{
    m_values.remove_all_matching([](Cell* key, Value) {
        return key->state() != Cell::State::Live;
    });
}

void WeakMap::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& entry : m_values)
        visitor.visit(entry.value);
}

}

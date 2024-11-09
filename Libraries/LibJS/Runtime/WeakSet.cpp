/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/WeakSet.h>

namespace JS {

JS_DEFINE_ALLOCATOR(WeakSet);

NonnullGCPtr<WeakSet> WeakSet::create(Realm& realm)
{
    return realm.heap().allocate<WeakSet>(realm, realm.intrinsics().weak_set_prototype());
}

WeakSet::WeakSet(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , WeakContainer(heap())
{
}

void WeakSet::remove_dead_cells(Badge<Heap>)
{
    m_values.remove_all_matching([](Cell* cell) {
        return cell->state() != Cell::State::Live;
    });
}

}

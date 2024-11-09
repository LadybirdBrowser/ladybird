/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/WeakContainer.h>

namespace JS {

class WeakSet final
    : public Object
    , public WeakContainer {
    JS_OBJECT(WeakSet, Object);
    JS_DECLARE_ALLOCATOR(WeakSet);

public:
    static NonnullGCPtr<WeakSet> create(Realm&);

    virtual ~WeakSet() override = default;

    HashTable<GCPtr<Cell>> const& values() const { return m_values; }
    HashTable<GCPtr<Cell>>& values() { return m_values; }

    virtual void remove_dead_cells(Badge<Heap>) override;

private:
    explicit WeakSet(Object& prototype);

    HashTable<RawGCPtr<Cell>> m_values; // This stores Cell pointers instead of Object pointers to aide with sweeping
};

}

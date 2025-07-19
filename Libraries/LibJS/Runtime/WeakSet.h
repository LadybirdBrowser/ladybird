/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibGC/WeakContainer.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class WeakSet final
    : public Object
    , public GC::WeakContainer {
    JS_OBJECT(WeakSet, Object);
    GC_DECLARE_ALLOCATOR(WeakSet);

public:
    static GC::Ref<WeakSet> create(Realm&);

    virtual ~WeakSet() override = default;

    HashTable<GC::Ptr<Cell>> const& values() const { return m_values; }
    HashTable<GC::Ptr<Cell>>& values() { return m_values; }

    virtual void remove_dead_cells(Badge<GC::Heap>) override;

private:
    explicit WeakSet(Object& prototype);

    HashTable<GC::RawPtr<Cell>> m_values; // This stores Cell pointers instead of Object pointers to aide with sweeping
};

}

/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibGC/WeakContainer.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API WeakSet final
    : public Object
    , public GC::WeakContainer {
    JS_OBJECT(WeakSet, Object);
    GC_DECLARE_ALLOCATOR(WeakSet);

public:
    static GC::Ref<WeakSet> create(Realm&);

    virtual ~WeakSet() override = default;

    bool weak_set_has(GC::Ptr<Cell>) const;
    void weak_set_add(GC::Ptr<Cell>);
    bool weak_set_remove(GC::Ptr<Cell>);
    size_t weak_set_size() const { return m_values.size(); }

    virtual void remove_dead_cells(Badge<GC::Heap>) override;
    virtual size_t external_memory_size() const override;

private:
    explicit WeakSet(Object& prototype);

    void account_external_memory_change(size_t old_external_memory_size);

    HashTable<GC::RawPtr<Cell>> m_values; // This stores Cell pointers instead of Object pointers to aide with sweeping
};

}

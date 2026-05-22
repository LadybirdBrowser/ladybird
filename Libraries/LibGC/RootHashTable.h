/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/IntrusiveList.h>
#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapRoot.h>
#include <LibGC/Rootable.h>

namespace GC {

class GC_API RootHashTableBase {
public:
    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>&) const = 0;

protected:
    RootHashTableBase();
    explicit RootHashTableBase(Heap&);
    ~RootHashTableBase();

    Heap* m_heap { nullptr };
    IntrusiveListNode<RootHashTableBase> m_list_node;

public:
    using List = IntrusiveList<&RootHashTableBase::m_list_node>;
};

template<typename T, typename TraitsForT = Traits<T>, bool IsOrdered = false>
class RootHashTable final
    : public RootHashTableBase
    , public HashTable<T, TraitsForT, IsOrdered> {

    using HashTableBase = HashTable<T, TraitsForT, IsOrdered>;

public:
    RootHashTable()
        : RootHashTableBase()
    {
    }

    ~RootHashTable() = default;

    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>& roots) const override
    {
        static_assert(Detail::RootableValueTraits<T>::is_rootable,
            "RootHashTable element type must be convertible to Cell const* or derive from NanBoxedValue");
        for (auto& value : *this)
            Detail::gather_root(roots, value, HeapRoot::Type::RootHashTable);
    }
};

template<typename T, typename TraitsForT = Traits<T>>
using OrderedRootHashTable = RootHashTable<T, TraitsForT, true>;

}

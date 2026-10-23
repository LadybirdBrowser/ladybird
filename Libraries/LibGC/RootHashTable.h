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
    AK_MAKE_NONCOPYABLE(RootHashTableBase);

public:
    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>&) const = 0;

protected:
    RootHashTableBase();
    explicit RootHashTableBase(Heap&);
    ~RootHashTableBase();

    void assign_heap(Heap*);

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

    RootHashTable(HashTableBase&& other)
        : RootHashTableBase()
        , HashTableBase(move(other))
    {
    }

    RootHashTable(RootHashTable const& other)
        : RootHashTableBase(*other.m_heap)
        , HashTableBase(static_cast<HashTableBase const&>(other))
    {
    }

    RootHashTable(RootHashTable&& other)
        : RootHashTableBase(*other.m_heap)
        , HashTableBase(move(static_cast<HashTableBase&>(other)))
    {
    }

    RootHashTable& operator=(RootHashTable const& other)
    {
        if (&other == this)
            return *this;

        assign_heap(other.m_heap);
        HashTableBase::operator=(static_cast<HashTableBase const&>(other));
        return *this;
    }

    RootHashTable& operator=(RootHashTable&& other)
    {
        assign_heap(other.m_heap);
        HashTableBase::operator=(move(static_cast<HashTableBase&>(other)));
        return *this;
    }

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

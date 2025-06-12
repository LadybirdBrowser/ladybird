/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/IntrusiveList.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapRoot.h>

namespace GC {

class RootHashMapBase {
public:
    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>&) const = 0;

protected:
    explicit RootHashMapBase(Heap&);
    ~RootHashMapBase();

    void assign_heap(Heap*);

    Heap* m_heap { nullptr };
    IntrusiveListNode<RootHashMapBase> m_list_node;

public:
    using List = IntrusiveList<&RootHashMapBase::m_list_node>;
};

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>, bool IsOrdered = false>
class RootHashMap final
    : public RootHashMapBase
    , public HashMap<K, V, KeyTraits, ValueTraits, IsOrdered> {

    using HashMapBase = HashMap<K, V, KeyTraits, ValueTraits, IsOrdered>;

public:
    explicit RootHashMap(Heap& heap)
        : RootHashMapBase(heap)
    {
    }

    ~RootHashMap() = default;

    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>& roots) const override
    {
        for (auto& [key, value] : *this) {
            if constexpr (IsBaseOf<NanBoxedValue, V>) {
                if (value.is_cell())
                    roots.set(&const_cast<V&>(value).as_cell(), HeapRoot { .type = HeapRoot::Type::RootHashMap });
            } else {
                roots.set(value, HeapRoot { .type = HeapRoot::Type::RootHashMap });
            }
        }
    }
};

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>>
using OrderedRootHashMap = RootHashMap<K, V, KeyTraits, ValueTraits, true>;

}

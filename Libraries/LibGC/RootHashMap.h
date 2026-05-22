/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/IntrusiveList.h>
#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapRoot.h>
#include <LibGC/Rootable.h>

namespace GC {

class GC_API RootHashMapBase {
public:
    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>&) const = 0;

protected:
    RootHashMapBase();
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
    RootHashMap()
        : RootHashMapBase()
    {
    }

    ~RootHashMap() = default;

    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>& roots) const override
    {
        static constexpr bool KeyIsGCType = Detail::RootableValueTraits<K>::is_rootable;
        static constexpr bool ValueIsGCType = Detail::RootableValueTraits<V>::is_rootable;
        static_assert(KeyIsGCType || ValueIsGCType,
            "RootHashMap requires at least one of key or value types to be convertible to Cell const* or derive from NanBoxedValue");
        for (auto& [key, value] : *this) {
            if constexpr (KeyIsGCType)
                Detail::gather_root(roots, key, HeapRoot::Type::RootHashMap);
            if constexpr (ValueIsGCType)
                Detail::gather_root(roots, value, HeapRoot::Type::RootHashMap);
        }
    }
};

template<typename K, typename V, typename KeyTraits = Traits<K>, typename ValueTraits = Traits<V>>
using OrderedRootHashMap = RootHashMap<K, V, KeyTraits, ValueTraits, true>;

}

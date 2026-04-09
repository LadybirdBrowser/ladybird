/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/IntrusiveList.h>
#include <LibGC/Forward.h>

namespace GC {

class GC_API ConservativeHashTableBase {
    AK_MAKE_NONCOPYABLE(ConservativeHashTableBase);

public:
    virtual void for_each_possible_value(AK::Function<void(FlatPtr)> callback) const = 0;

protected:
    ConservativeHashTableBase();
    explicit ConservativeHashTableBase(Heap&);
    ~ConservativeHashTableBase();

    Heap* m_heap { nullptr };
    IntrusiveListNode<ConservativeHashTableBase> m_list_node;

public:
    using List = IntrusiveList<&ConservativeHashTableBase::m_list_node>;
};

template<typename T, typename TraitsForT = Traits<T>, bool IsOrdered = false>
class GC_API ConservativeHashTable final
    : public ConservativeHashTableBase
    , public HashTable<T, TraitsForT, IsOrdered> {

    using HashTableBase = HashTable<T, TraitsForT, IsOrdered>;

public:
    ConservativeHashTable()
        : ConservativeHashTableBase()
    {
    }

    ConservativeHashTable(ConservativeHashTable const& other)
        : ConservativeHashTableBase(*other.m_heap)
        , HashTableBase(static_cast<HashTableBase const&>(other))
    {
    }

    ConservativeHashTable(ConservativeHashTable&& other)
        : ConservativeHashTableBase(*other.m_heap)
        , HashTableBase(move(static_cast<HashTableBase&>(other)))
    {
    }

    ConservativeHashTable& operator=(ConservativeHashTable const& other)
    {
        if (&other == this)
            return *this;
        HashTableBase::operator=(static_cast<HashTableBase const&>(other));
        return *this;
    }

    ~ConservativeHashTable() = default;

    virtual void for_each_possible_value(AK::Function<void(FlatPtr)> callback) const override
    {
        for (auto& entry : *this) {
            auto entry_bytes = ReadonlyBytes { &entry, sizeof(T) };
            for (size_t i = 0; i + sizeof(FlatPtr) <= entry_bytes.size(); i += sizeof(FlatPtr)) {
                FlatPtr value;
                memcpy(&value, entry_bytes.offset(i), sizeof(FlatPtr));
                callback(value);
            }
        }
    }
};

template<typename T, typename TraitsForT = Traits<T>>
using OrderedConservativeHashTable = ConservativeHashTable<T, TraitsForT, true>;

}

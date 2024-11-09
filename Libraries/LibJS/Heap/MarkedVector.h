/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/IntrusiveList.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/HeapRoot.h>

namespace JS {

class MarkedVectorBase {
public:
    virtual void gather_roots(HashMap<Cell*, JS::HeapRoot>&) const = 0;

protected:
    explicit MarkedVectorBase(Heap&);
    ~MarkedVectorBase();

    MarkedVectorBase& operator=(MarkedVectorBase const&);

    Heap* m_heap { nullptr };
    IntrusiveListNode<MarkedVectorBase> m_list_node;

public:
    using List = IntrusiveList<&MarkedVectorBase::m_list_node>;
};

template<typename T, size_t inline_capacity>
class MarkedVector final
    : public MarkedVectorBase
    , public Vector<T, inline_capacity> {

public:
    explicit MarkedVector(Heap& heap)
        : MarkedVectorBase(heap)
    {
    }

    virtual ~MarkedVector() = default;

    MarkedVector(MarkedVector const& other)
        : MarkedVectorBase(*other.m_heap)
        , Vector<T, inline_capacity>(other)
    {
    }

    MarkedVector(MarkedVector&& other)
        : MarkedVectorBase(*other.m_heap)
        , Vector<T, inline_capacity>(move(static_cast<Vector<T, inline_capacity>&>(other)))
    {
    }

    MarkedVector& operator=(MarkedVector const& other)
    {
        Vector<T, inline_capacity>::operator=(other);
        MarkedVectorBase::operator=(other);
        return *this;
    }

    virtual void gather_roots(HashMap<Cell*, JS::HeapRoot>& roots) const override
    {
        for (auto& value : *this) {
            if constexpr (IsSame<Value, T>) {
                if (value.is_cell())
                    roots.set(&const_cast<T&>(value).as_cell(), HeapRoot { .type = HeapRoot::Type::MarkedVector });
            } else {
                roots.set(value, HeapRoot { .type = HeapRoot::Type::MarkedVector });
            }
        }
    }
};

}

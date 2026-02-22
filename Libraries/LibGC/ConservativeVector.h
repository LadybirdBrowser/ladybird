/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>

namespace GC {

class GC_API ConservativeVectorBase {
    AK_MAKE_NONCOPYABLE(ConservativeVectorBase);

public:
    virtual ReadonlySpan<FlatPtr> possible_values() const = 0;

protected:
    explicit ConservativeVectorBase(Heap&);
    ~ConservativeVectorBase();

    Heap* m_heap { nullptr };
    IntrusiveListNode<ConservativeVectorBase> m_list_node;

public:
    using List = IntrusiveList<&ConservativeVectorBase::m_list_node>;
};

template<typename T, size_t inline_capacity>
class GC_API ConservativeVector final
    : public ConservativeVectorBase
    , public Vector<T, inline_capacity> {

public:
    explicit ConservativeVector(Heap& heap)
        : ConservativeVectorBase(heap)
    {
    }

    ConservativeVector(Heap& heap, Vector<T, inline_capacity> const& other)
        : ConservativeVectorBase(heap)
        , Vector<T, inline_capacity>(other)
    {
    }

    ~ConservativeVector() = default;

    ConservativeVector(ConservativeVector const& other)
        : ConservativeVectorBase(*other.m_heap)
        , Vector<T, inline_capacity>(other)
    {
    }

    ConservativeVector(ConservativeVector&& other)
        : ConservativeVectorBase(*other.m_heap)
        , Vector<T, inline_capacity>(move(static_cast<Vector<T, inline_capacity>&>(other)))
    {
    }

    ConservativeVector& operator=(ConservativeVector const& other)
    {
        if (&other == this)
            return *this;
        Vector<T, inline_capacity>::operator=(other);
        return *this;
    }

    virtual ReadonlySpan<FlatPtr> possible_values() const override
    {
        static_assert(sizeof(T) >= sizeof(FlatPtr));
        return ReadonlySpan<FlatPtr> {
            reinterpret_cast<FlatPtr const*>(this->data()),
            this->size() * sizeof(T) / sizeof(FlatPtr),
        };
    }
};

}

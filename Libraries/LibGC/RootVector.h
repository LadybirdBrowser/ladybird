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
#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapRoot.h>
#include <LibGC/Rootable.h>

namespace GC {

class GC_API RootVectorBase {
public:
    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>&) const = 0;

protected:
    RootVectorBase();
    explicit RootVectorBase(Heap&);
    ~RootVectorBase();

    void assign_heap(Heap*);

    Heap* m_heap { nullptr };
    IntrusiveListNode<RootVectorBase> m_list_node;

public:
    using List = IntrusiveList<&RootVectorBase::m_list_node>;
};

template<typename T, size_t inline_capacity>
class RootVector final
    : public RootVectorBase
    , public Vector<T, inline_capacity> {

    using VectorBase = Vector<T, inline_capacity>;

public:
    RootVector()
        : RootVectorBase()
    {
    }

    ~RootVector() = default;

    RootVector(ReadonlySpan<T> other)
        : RootVectorBase()
        , Vector<T, inline_capacity>(other)
    {
    }

    RootVector(RootVector const& other)
        : RootVectorBase(*other.m_heap)
        , Vector<T, inline_capacity>(other)
    {
    }

    RootVector(RootVector&& other)
        : RootVectorBase(*other.m_heap)
        , VectorBase(move(static_cast<VectorBase&>(other)))
    {
    }

    RootVector& operator=(RootVector const& other)
    {
        if (&other == this)
            return *this;

        assign_heap(other.m_heap);
        VectorBase::operator=(other);
        return *this;
    }

    RootVector& operator=(RootVector&& other)
    {
        assign_heap(other.m_heap);
        VectorBase::operator=(move(static_cast<VectorBase&>(other)));
        return *this;
    }

    virtual void gather_roots(HashMap<Cell*, GC::HeapRoot>& roots) const override
    {
        static_assert(Detail::RootableValueTraits<T>::is_rootable,
            "RootVector element type must be convertible to Cell const* or derive from NanBoxedValue");
        for (auto& value : *this)
            Detail::gather_root(roots, value, HeapRoot::Type::RootVector);
    }
};

template<typename T>
RootVector(ReadonlySpan<T> const&) -> RootVector<T>;

template<typename T>
RootVector(Span<T> const&) -> RootVector<T>;

template<typename T>
RootVector(Vector<T> const&) -> RootVector<T>;

}

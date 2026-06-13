/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/Forward.h>
#include <LibGC/Internals.h>

namespace GC {

// A strong reference from a cell on one heap to a cell on another heap in the same HeapGroup.
class GC_API CrossHeapMemberBase {
    AK_MAKE_NONCOPYABLE(CrossHeapMemberBase);
    AK_MAKE_NONMOVABLE(CrossHeapMemberBase);

public:
    Cell* cell_base() const { return static_cast<Cell*>(m_cell); }

    void detach_from_heap(Badge<Heap>)
    {
        m_registered_heap = nullptr;
        m_cell = nullptr;
    }

protected:
    explicit CrossHeapMemberBase(Cell* cell)
    {
        reset(cell);
    }

    ~CrossHeapMemberBase()
    {
        reset(nullptr);
    }

    void reset(Cell* cell);

    void* m_cell { nullptr }; // Not a member of "this" heap.
    Heap* m_registered_heap { nullptr };
};

template<typename T>
class CrossHeapMember final : public CrossHeapMemberBase {
public:
    CrossHeapMember()
        : CrossHeapMemberBase(nullptr)
    {
    }

    explicit CrossHeapMember(T* cell)
        : CrossHeapMemberBase(cell)
    {
    }

    CrossHeapMember& operator=(T* cell)
    {
        reset(cell);
        return *this;
    }

    T* ptr() const { return static_cast<T*>(m_cell); }
    T* operator->() const { return ptr(); }
    explicit operator bool() const { return m_cell != nullptr; }

    // Called by the holder heap.
    void visit(Cell::Visitor& visitor) const
    {
        if (m_cell)
            visitor.visit(cell_base());
    }
};

}

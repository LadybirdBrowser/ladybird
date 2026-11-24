/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>

namespace GC {

// A vector of cells that does not keep them alive. It must be owned by a WeakContainer, which calls
// remove_dead_cells() from its own remove_dead_cells().
template<typename T>
class WeakVector {
public:
    WeakVector() = default;

    explicit WeakVector(ReadonlySpan<Ref<T>> cells)
    {
        m_cells.ensure_capacity(cells.size());
        for (auto cell : cells)
            m_cells.unchecked_append(cell.ptr());
    }

    class ConstIterator {
    public:
        bool operator==(ConstIterator const& other) const { return m_cell == other.m_cell; }

        ConstIterator& operator++()
        {
            ++m_cell;
            return *this;
        }

        T& operator*() const { return **m_cell; }

    private:
        friend class WeakVector;

        explicit ConstIterator(T* const* cell)
            : m_cell(cell)
        {
        }

        T* const* m_cell;
    };

    ConstIterator begin() const { return ConstIterator(m_cells.data()); }
    ConstIterator end() const { return ConstIterator(m_cells.data() + m_cells.size()); }

    size_t size() const { return m_cells.size(); }
    size_t capacity() const { return m_cells.capacity(); }
    bool is_empty() const { return m_cells.is_empty(); }
    T& operator[](size_t index) const { return *m_cells[index]; }
    T& first() const { return *m_cells.first(); }

    bool contains(T const& cell) const
    {
        return m_cells.contains_slow(const_cast<T*>(&cell));
    }

    void append(T& cell) { m_cells.append(&cell); }
    void ensure_capacity(size_t capacity) { m_cells.ensure_capacity(capacity); }
    void clear() { m_cells.clear(); }
    void clear_with_capacity() { m_cells.clear_with_capacity(); }
    void shrink_to_fit() { m_cells.shrink_to_fit(); }

    // Returns whether any cell was removed.
    bool remove_dead_cells(Heap const& heap)
    {
        return m_cells.remove_all_matching([&](T* cell) {
            return !heap.is_live_cell(cell) || !cell->is_marked();
        });
    }

private:
    Vector<T*> m_cells;
};

}

/*
 * Copyright (c) 2026, Marc Butler <marc@mailworks.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/WriteBarrier.h>

namespace GC {

// A Vector<Value> wrapper that calls the Yuasa write barrier before
// overwriting any element that might contain a cell pointer.
// Read access is unrestricted; mutation goes through set() or
// explicit barrier-aware methods.
template<typename V>
class ValueVector {
    static_assert(IsBaseOf<NanBoxedValue, V>);

public:
    ValueVector() = default;

    ValueVector(Vector<V>&& vec)
        : m_vector(move(vec))
    {
    }

    // --- Read-only element access ---

    V const& operator[](size_t i) const { return m_vector[i]; }
    V const& at(size_t i) const { return m_vector.at(i); }
    V const* data() const { return m_vector.data(); }

    size_t size() const { return m_vector.size(); }
    bool is_empty() const { return m_vector.is_empty(); }

    auto begin() const { return m_vector.begin(); }
    auto end() const { return m_vector.end(); }
    ReadonlySpan<V> span() const { return m_vector.span(); }
    operator ReadonlySpan<V>() const { return m_vector.span(); }

    // --- Barriered mutation ---

    void set(size_t i, V value)
    {
        value_write_barrier(m_vector[i], value);
        m_vector[i] = value;
    }

    // Append: no old value, but barrier the new value for Dijkstra insertion.
    void append(V value)
    {
        value_write_barrier(V(), value);
        m_vector.append(value);
    }

    void ensure_capacity(size_t c) { m_vector.ensure_capacity(c); }

    void resize(size_t new_size)
    {
        // Barrier elements being truncated.
        for (size_t i = new_size; i < m_vector.size(); ++i)
            value_write_barrier(m_vector[i]);
        m_vector.resize(new_size);
    }

    void clear()
    {
        for (auto& v : m_vector)
            value_write_barrier(v);
        m_vector.clear();
    }

    void resize_and_keep_capacity(size_t new_size)
    {
        for (size_t i = new_size; i < m_vector.size(); ++i)
            value_write_barrier(m_vector[i]);
        m_vector.resize_and_keep_capacity(new_size);
    }

    // --- Visitor support ---
    // Expose underlying vector so visit_edges can use visitor.visit(vec.underlying()).
    Vector<V> const& underlying() const { return m_vector; }

private:
    Vector<V> m_vector;
};

}

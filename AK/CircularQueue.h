/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Forward.h>
#include <AK/StdLibExtras.h>

namespace AK {

template<typename T, size_t Capacity>
class CircularQueue {
public:
    CircularQueue() = default;

    ~CircularQueue()
    {
        clear();
    }

    void clear()
    {
        for (size_t i = 0; i < m_size; ++i)
            elements()[(m_head + i) % Capacity].~T();

        m_head = 0;
        m_size = 0;
    }

    bool is_empty() const { return m_size == 0; }
    size_t size() const { return m_size; }

    size_t capacity() const { return Capacity; }

    template<typename U = T>
    void enqueue(U&& value)
    {
        auto& slot = elements()[(m_head + m_size) % Capacity];
        if (m_size == Capacity)
            slot.~T();

        new (&slot) T(forward<U>(value));
        if (m_size == Capacity)
            m_head = (m_head + 1) % Capacity;
        else
            ++m_size;
    }

    T dequeue()
    {
        VERIFY(!is_empty());
        auto& slot = elements()[m_head];
        T value = move(slot);
        slot.~T();
        m_head = (m_head + 1) % Capacity;
        --m_size;
        return value;
    }

    T const& at(size_t index) const { return elements()[(m_head + index) % Capacity]; }
    T& at(size_t index) { return elements()[(m_head + index) % Capacity]; }

    T const& first() const { return at(0); }
    T const& last() const { return at(size() - 1); }

    class ConstIterator {
    public:
        bool operator!=(ConstIterator const& other) { return m_index != other.m_index; }
        ConstIterator& operator++()
        {
            ++m_index;
            return *this;
        }

        T const& operator*() const { return m_queue.at(m_index); }

    private:
        friend class CircularQueue;
        ConstIterator(CircularQueue const& queue, size_t const index)
            : m_queue(queue)
            , m_index(index)
        {
        }
        CircularQueue const& m_queue;
        size_t m_index { 0 };
    };

    class Iterator {
    public:
        bool operator!=(Iterator const& other) { return m_index != other.m_index; }
        Iterator& operator++()
        {
            ++m_index;
            return *this;
        }

        T& operator*() const { return m_queue.at(m_index); }

    private:
        friend class CircularQueue;
        Iterator(CircularQueue& queue, size_t const index)
            : m_queue(queue)
            , m_index(index)
        {
        }
        CircularQueue& m_queue;
        size_t m_index { 0 };
    };

    ConstIterator begin() const { return ConstIterator(*this, 0); }
    ConstIterator end() const { return ConstIterator(*this, size()); }

    Iterator begin() { return Iterator(*this, 0); }
    Iterator end() { return Iterator(*this, size()); }

    size_t head_index() const { return m_head; }

protected:
    T* elements() { return reinterpret_cast<T*>(m_storage); }
    T const* elements() const { return reinterpret_cast<T const*>(m_storage); }

    friend class ConstIterator;
    alignas(T) u8 m_storage[sizeof(T) * Capacity];
    size_t m_size { 0 };
    size_t m_head { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::CircularQueue;
#endif

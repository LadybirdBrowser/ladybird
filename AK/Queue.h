/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <AK/OwnPtr.h>
#include <AK/SinglyLinkedList.h>
#include <AK/Vector.h>

namespace AK {

template<typename T, int segment_size = 1000>
class Queue {
public:
    Queue() = default;

    ~Queue()
    {
        clear();
    }

    size_t size() const { return m_size; }
    bool is_empty() const { return m_size == 0; }

    template<typename U = T>
    void enqueue(U&& value)
    {
        if (m_segments.is_empty() || m_segments.last()->data.size() >= segment_size) {
            auto segment = new QueueSegment;
            m_segments.append(*segment);
        }
        m_segments.last()->data.append(forward<U>(value));
        ++m_size;
    }

    ErrorOr<T> try_dequeue()
    {
        if (is_empty())
            return Error::from_errno(ENOENT);

        return dequeue();
    }

    T dequeue()
    {
        VERIFY(!is_empty());
        auto value = move(m_segments.first()->data[m_index_into_first++]);
        if (m_index_into_first == segment_size) {
            delete m_segments.take_first();
            m_index_into_first = 0;
        }
        --m_size;
        if (m_size == 0 && !m_segments.is_empty()) {
            // This is not necessary for correctness but avoids faulting in
            // all the pages for the underlying Vector in the case where
            // the caller repeatedly enqueues and then dequeues a single item.
            m_index_into_first = 0;
            m_segments.last()->data.clear_with_capacity();
        }
        return value;
    }

    T const& head() const
    {
        VERIFY(!is_empty());
        return m_segments.first()->data[m_index_into_first];
    }

    T& tail()
    {
        VERIFY(!is_empty());
        return m_segments.last()->data.last();
    }

    void clear()
    {
        while (auto* segment = m_segments.take_first())
            delete segment;
        m_index_into_first = 0;
        m_size = 0;
    }

private:
    struct QueueSegment {
        Vector<T, segment_size> data;
        IntrusiveListNode<QueueSegment> node;
    };

    IntrusiveList<&QueueSegment::node> m_segments;
    size_t m_index_into_first { 0 };
    size_t m_size { 0 };
};

}

#if USING_AK_GLOBALLY
using AK::Queue;
#endif

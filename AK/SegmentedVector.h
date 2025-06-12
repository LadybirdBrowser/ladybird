/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Vector.h>

namespace AK {

template<typename T, int segment_size = 512>
class SegmentedVector {
private:
    using VisibleType = RemoveReference<T>;
    static constexpr bool contains_reference = IsLvalueReference<T>;

public:
    SegmentedVector() = default;

    size_t size() const { return m_size; }
    bool is_empty() const { return m_size == 0; }

    using Iterator = SimpleIterator<SegmentedVector, VisibleType>;

    Iterator begin() { return Iterator::begin(*this); }
    Iterator end() { return Iterator::end(*this); }

    ALWAYS_INLINE VisibleType const& at(size_t i) const
    {
        VERIFY(i < m_size);
        auto segment_index = i / segment_size;
        auto index_in_segment = i % segment_size;
        return m_segments[segment_index]->at(index_in_segment);
    }

    ALWAYS_INLINE VisibleType& at(size_t i)
    {
        VERIFY(i < m_size);
        auto segment_index = i / segment_size;
        auto index_in_segment = i % segment_size;
        return m_segments[segment_index]->at(index_in_segment);
    }

    ALWAYS_INLINE VisibleType const& operator[](size_t i) const { return at(i); }
    ALWAYS_INLINE VisibleType& operator[](size_t i) { return at(i); }

    void append(T&& value)
    {
        if (m_segments.is_empty() || m_segments.last()->size() >= segment_size)
            m_segments.append(make<Vector<T, segment_size>>());

        if constexpr (contains_reference) {
            m_segments.last()->append(value);
        } else {
            m_segments.last()->append(move(value));
        }
        ++m_size;
    }

private:
    Vector<NonnullOwnPtr<Vector<T, segment_size>>> m_segments;
    size_t m_size { 0 };
};

}

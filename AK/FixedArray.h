/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Iterator.h>
#include <AK/Span.h>
#include <AK/kmalloc.h>
#include <initializer_list>

namespace AK {

// FixedArray is an Array with a size only known at run-time.
// It guarantees to only allocate when being constructed, and to only deallocate when being destructed.
template<typename T>
class FixedArray {
public:
    FixedArray() = default;

    static ErrorOr<FixedArray<T>> create(std::initializer_list<T> initializer)
    {
        auto array = TRY(create(initializer.size()));
        auto it = initializer.begin();
        for (size_t i = 0; i < array.size(); ++i) {
            array[i] = move(*it);
            ++it;
        }
        return array;
    }

    static ErrorOr<FixedArray<T>> create(size_t size)
    {
        if (size == 0)
            return FixedArray<T>();
        auto* elements = reinterpret_cast<T*>(kmalloc(storage_allocation_size(size)));
        if (!elements)
            return Error::from_errno(ENOMEM);
        for (size_t i = 0; i < size; ++i)
            new (&elements[i]) T();
        return FixedArray<T>(size, elements);
    }

    static FixedArray<T> must_create_but_fixme_should_propagate_errors(size_t size)
    {
        return MUST(create(size));
    }

    template<size_t N>
    static ErrorOr<FixedArray<T>> create(T (&&array)[N])
    {
        return create(Span(array, N));
    }

    template<typename U>
    static ErrorOr<FixedArray<T>> create(Span<U> span)
    {
        if (span.size() == 0)
            return FixedArray<T>();
        auto* elements = reinterpret_cast<T*>(kmalloc(storage_allocation_size(span.size())));
        if (!elements)
            return Error::from_errno(ENOMEM);
        for (size_t i = 0; i < span.size(); ++i)
            new (&elements[i]) T(span[i]);
        return FixedArray<T>(span.size(), elements);
    }

    ErrorOr<FixedArray<T>> clone() const
    {
        return create(span());
    }

    // Nobody can ever use these functions, since it would be impossible to make them OOM-safe due to their signatures. We just explicitly delete them.
    FixedArray(FixedArray<T> const&) = delete;
    FixedArray<T>& operator=(FixedArray<T> const&) = delete;

    FixedArray(FixedArray<T>&& other)
        : m_size(exchange(other.m_size, 0))
        , m_elements(exchange(other.m_elements, nullptr))
    {
    }

    FixedArray<T>& operator=(FixedArray<T>&& other)
    {
        if (this != &other) {
            this->~FixedArray();
            new (this) FixedArray<T>(move(other));
        }
        return *this;
    }

    ~FixedArray()
    {
        if (!m_elements)
            return;
        for (size_t i = 0; i < m_size; ++i)
            m_elements[i].~T();
        kfree_sized(m_elements, storage_allocation_size(m_size));
        m_elements = nullptr;
    }

    size_t size() const { return m_size; }
    bool is_empty() const { return size() == 0; }
    T* data() { return m_elements; }
    T const* data() const { return m_elements; }

    T& at(size_t index)
    {
        VERIFY(index < m_size);
        return m_elements[index];
    }

    T& unchecked_at(size_t index)
    {
        return m_elements[index];
    }

    T const& at(size_t index) const
    {
        VERIFY(index < m_size);
        return m_elements[index];
    }

    T& operator[](size_t index)
    {
        return at(index);
    }

    T const& operator[](size_t index) const
    {
        return at(index);
    }

    bool contains_slow(T const& value) const
    {
        if (!m_elements)
            return false;
        for (size_t i = 0; i < m_size; ++i) {
            if (m_elements[i] == value)
                return true;
        }
        return false;
    }

    void swap(FixedArray<T>& other)
    {
        AK::swap(m_size, other.m_size);
        AK::swap(m_elements, other.m_elements);
    }

    void fill_with(T const& value)
    {
        if (!m_elements)
            return;
        for (size_t i = 0; i < m_size; ++i)
            m_elements[i] = value;
    }

    using Iterator = SimpleIterator<FixedArray, T>;
    using ConstIterator = SimpleIterator<FixedArray const, T const>;

    Iterator begin() { return Iterator::begin(*this); }
    ConstIterator begin() const { return ConstIterator::begin(*this); }

    Iterator end() { return Iterator::end(*this); }
    ConstIterator end() const { return ConstIterator::end(*this); }

    Span<T> span() { return { data(), size() }; }
    ReadonlySpan<T> span() const { return { data(), size() }; }

private:
    static size_t storage_allocation_size(size_t size)
    {
        return size * sizeof(T);
    }

    FixedArray(size_t size, T* elements)
        : m_size(size)
        , m_elements(elements)
    {
    }

    size_t m_size { 0 };
    T* m_elements { nullptr };
};

}

#if USING_AK_GLOBALLY
using AK::FixedArray;
#endif

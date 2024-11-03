/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <iterator>

namespace AK {

template<typename Container, typename ValueType>
class SimpleIterator {
public:
    friend Container;

    using value_type = ValueType;
    using iterator_category = std::random_access_iterator_tag;

    constexpr bool is_end() const { return m_index == SimpleIterator::end(*m_container).m_index; }
    constexpr size_t index() const { return m_index; }

    constexpr bool operator==(SimpleIterator const& other) const = default;
    constexpr auto operator<=>(SimpleIterator const& other) const = default;

    constexpr SimpleIterator operator+(ptrdiff_t delta) const { return SimpleIterator { *m_container, m_index + delta }; }
    friend constexpr SimpleIterator operator+(ptrdiff_t delta, SimpleIterator const& it) { return it + delta; }
    constexpr SimpleIterator& operator+=(ptrdiff_t delta)
    {
        m_index += delta;
        return *this;
    }
    constexpr SimpleIterator operator-(ptrdiff_t delta) const { return SimpleIterator { *m_container, m_index - delta }; }
    friend constexpr SimpleIterator operator-(ptrdiff_t delta, SimpleIterator const& it) { return it - delta; }
    constexpr SimpleIterator& operator-=(ptrdiff_t delta)
    {
        m_index -= delta;
        return *this;
    }

    constexpr ValueType& operator[](ptrdiff_t delta) const
    requires(!IsConst<ValueType>)
    {
        return (*m_container)[m_index + delta];
    }
    constexpr ValueType const& operator[](ptrdiff_t delta) const
    requires(IsConst<ValueType>)
    {
        return (*m_container)[m_index + delta];
    }

    constexpr ptrdiff_t operator-(SimpleIterator other) const { return static_cast<ptrdiff_t>(m_index) - other.m_index; }

    constexpr SimpleIterator& operator++()
    {
        ++m_index;
        return *this;
    }
    constexpr SimpleIterator operator++(int)
    {
        ++m_index;
        return SimpleIterator { *m_container, m_index - 1 };
    }

    constexpr SimpleIterator& operator--()
    {
        --m_index;
        return *this;
    }
    constexpr SimpleIterator operator--(int)
    {
        --m_index;
        return SimpleIterator { *m_container, m_index + 1 };
    }

    [[nodiscard]] ALWAYS_INLINE constexpr ValueType const& operator*() const
    requires(IsConst<ValueType>)
    {
        return (*m_container)[m_index];
    }
    [[nodiscard]] ALWAYS_INLINE constexpr ValueType& operator*() const
    requires(!IsConst<ValueType>)
    {
        return (*m_container)[m_index];
    }

    ALWAYS_INLINE constexpr ValueType const* operator->() const
    requires(IsConst<ValueType>)
    {
        return &(*m_container)[m_index];
    }
    ALWAYS_INLINE constexpr ValueType* operator->() const
    requires(!IsConst<ValueType>)
    {
        return &(*m_container)[m_index];
    }

    constexpr SimpleIterator& operator=(SimpleIterator const& other) = default;
    constexpr SimpleIterator(SimpleIterator const& obj) = default;

    constexpr SimpleIterator() = default;

private:
    static constexpr SimpleIterator begin(Container& container) { return { container, 0 }; }
    static constexpr SimpleIterator end(Container& container)
    {
        using RawContainerType = RemoveCV<Container>;

        if constexpr (IsSame<StringView, RawContainerType> || IsSame<ByteString, RawContainerType>)
            return { container, container.length() };
        else
            return { container, container.size() };
    }

    constexpr SimpleIterator(Container& container, size_t index)
        : m_container(&container)
        , m_index(index)
    {
    }

    Container* m_container = nullptr;
    size_t m_index = 0;
};

}

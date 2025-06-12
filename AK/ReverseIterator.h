/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>

namespace AK {

template<typename Container, typename ValueType>
class SimpleReverseIterator {
public:
    friend Container;

    constexpr bool is_end() const { return m_index == SimpleReverseIterator::rend(m_container).m_index; }
    constexpr int index() const { return m_index; }

    constexpr bool operator==(SimpleReverseIterator other) const { return m_index == other.m_index; }
    constexpr bool operator!=(SimpleReverseIterator other) const { return m_index != other.m_index; }
    constexpr bool operator<(SimpleReverseIterator other) const { return m_index < other.m_index; }
    constexpr bool operator>(SimpleReverseIterator other) const { return m_index > other.m_index; }
    constexpr bool operator<=(SimpleReverseIterator other) const { return m_index <= other.m_index; }
    constexpr bool operator>=(SimpleReverseIterator other) const { return m_index >= other.m_index; }

    constexpr SimpleReverseIterator operator+(int delta) const { return SimpleReverseIterator { m_container, m_index - delta }; }
    constexpr SimpleReverseIterator operator-(int delta) const { return SimpleReverseIterator { m_container, m_index + delta }; }

    constexpr SimpleReverseIterator operator++()
    {
        --m_index;
        return *this;
    }
    constexpr SimpleReverseIterator operator++(int)
    {
        --m_index;
        return SimpleReverseIterator { m_container, m_index + 1 };
    }
    constexpr SimpleReverseIterator operator--()
    {
        ++m_index;
        return *this;
    }
    constexpr SimpleReverseIterator operator--(int)
    {
        ++m_index;
        return SimpleReverseIterator { m_container, m_index - 1 };
    }

    ALWAYS_INLINE constexpr ValueType const& operator*() const { return m_container[m_index]; }
    ALWAYS_INLINE constexpr ValueType& operator*() { return m_container[m_index]; }

    ALWAYS_INLINE constexpr ValueType const* operator->() const { return &m_container[m_index]; }
    ALWAYS_INLINE constexpr ValueType* operator->() { return &m_container[m_index]; }

    SimpleReverseIterator& operator=(SimpleReverseIterator const& other)
    {
        m_index = other.m_index;
        return *this;
    }
    SimpleReverseIterator(SimpleReverseIterator const& obj) = default;

private:
    static constexpr SimpleReverseIterator rbegin(Container& container)
    {
        using RawContainerType = RemoveCV<Container>;
        if constexpr (IsSame<StringView, RawContainerType> || IsSame<ByteString, RawContainerType>)
            return { container, static_cast<int>(container.length()) - 1 };
        else
            return { container, static_cast<int>(container.size()) - 1 };
    }

    static constexpr SimpleReverseIterator rend(Container& container)
    {
        return { container, -1 };
    }

    constexpr SimpleReverseIterator(Container& container, int index)
        : m_container(container)
        , m_index(index)
    {
    }

    Container& m_container;
    int m_index { 0 };
};

namespace ReverseWrapper {

template<typename Container>
struct ReverseWrapper {
    Container& container;
};

template<typename Container>
auto begin(ReverseWrapper<Container> wrapper) { return wrapper.container.rbegin(); }

template<typename Container>
auto end(ReverseWrapper<Container> wrapper) { return wrapper.container.rend(); }

template<typename Container>
ReverseWrapper<Container> in_reverse(Container&& container) { return { container }; }

}

}

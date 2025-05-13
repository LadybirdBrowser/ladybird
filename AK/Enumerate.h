/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>

namespace AK {

namespace Detail {

template<typename Iterable>
class Enumerator {
    using IteratorType = decltype(declval<Iterable>().begin());
    using ValueType = decltype(*declval<IteratorType>());

    struct Enumeration {
        size_t index { 0 };
        ValueType value;
    };

public:
    Enumerator(Iterable&& iterable)
        : m_iterable(forward<Iterable>(iterable))
        , m_iterator(m_iterable.begin())
        , m_end(m_iterable.end())
    {
    }

    Enumerator const& begin() const { return *this; }
    Enumerator const& end() const { return *this; }

    Enumeration operator*() { return { m_index, *m_iterator }; }
    Enumeration operator*() const { return { m_index, *m_iterator }; }

    bool operator!=(Enumerator const&) const { return m_iterator != m_end; }

    void operator++()
    {
        ++m_index;
        ++m_iterator;
    }

private:
    Iterable m_iterable;

    size_t m_index { 0 };
    IteratorType m_iterator;
    IteratorType const m_end;
};

}

template<typename T>
auto enumerate(T&& range)
{
    return Detail::Enumerator<T> { forward<T>(range) };
}

}

#ifdef USING_AK_GLOBALLY
using AK::enumerate;
#endif

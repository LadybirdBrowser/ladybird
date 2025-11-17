/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>

namespace AK {

namespace Detail {

template<typename Iterable>
struct Enumerator {
    using IteratorType = decltype(declval<Iterable>().begin());
    using ValueType = decltype(*declval<IteratorType>());

    struct Enumeration {
        size_t index { 0 };
        ValueType value;
    };

    struct Iterator {
        Enumeration operator*() { return { index, *iterator }; }
        Enumeration operator*() const { return { index, *iterator }; }

        bool operator!=(Iterator const& other) const { return iterator != other.iterator; }

        void operator++()
        {
            ++index;
            ++iterator;
        }

        size_t index { 0 };
        IteratorType iterator;
    };

    Iterator begin() { return { 0, iterable.begin() }; }
    Iterator end() { return { 0, iterable.end() }; }

    Iterable iterable;
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

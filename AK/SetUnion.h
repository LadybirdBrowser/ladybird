/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/StdLibExtras.h>

namespace AK {

template<typename Collection>
concept SortedCollection = requires(Collection const& collection, size_t index) {
    collection.size();
    collection[index];
};

namespace Detail {

template<SortedCollection Collection, typename Comparator>
constexpr bool is_sorted_without_duplicates(Collection const& collection, Comparator const& less_than)
{
    for (size_t index = 1; index < collection.size(); ++index) {
        if (!less_than(collection[index - 1], collection[index]))
            return false;
    }
    return true;
}

}

template<SortedCollection Left, SortedCollection Right, typename Output, typename Comparator>
void set_union(Left const& left, Right const& right, Output& output, Comparator less_than)
{
    VERIFY(static_cast<void const*>(&output) != static_cast<void const*>(&left));
    VERIFY(static_cast<void const*>(&output) != static_cast<void const*>(&right));
    ASSERT(Detail::is_sorted_without_duplicates(left, less_than));
    ASSERT(Detail::is_sorted_without_duplicates(right, less_than));
    size_t left_index = 0;
    size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        if (less_than(left[left_index], right[right_index])) {
            output.append(left[left_index++]);
        } else if (less_than(right[right_index], left[left_index])) {
            output.append(right[right_index++]);
        } else {
            output.append(left[left_index++]);
            ++right_index;
        }
    }
    for (; left_index < left.size(); ++left_index)
        output.append(left[left_index]);
    for (; right_index < right.size(); ++right_index)
        output.append(right[right_index]);
}

template<SortedCollection Left, SortedCollection Right, typename Output>
void set_union(Left const& left, Right const& right, Output& output)
{
    set_union(left, right, output, [](auto const& a, auto const& b) { return a < b; });
}

}

#if USING_AK_GLOBALLY
using AK::set_union;
#endif

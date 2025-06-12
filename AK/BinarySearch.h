/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace AK {

struct DefaultComparator {
    template<typename T, typename S>
    [[nodiscard]] constexpr int operator()(T& lhs, S& rhs)
    {
        if (lhs > rhs)
            return 1;

        if (lhs < rhs)
            return -1;

        return 0;
    }
};

template<typename Container, typename Needle, typename Comparator = DefaultComparator>
constexpr auto binary_search(
    Container&& haystack,
    Needle&& needle,
    size_t* nearby_index = nullptr,
    Comparator comparator = Comparator {}) -> decltype(&haystack[0])
{
    if (haystack.size() == 0) {
        if (nearby_index)
            *nearby_index = 0;
        return nullptr;
    }

    size_t low = 0;
    size_t high = haystack.size() - 1;
    while (low <= high) {
        size_t middle = low + (high - low) / 2;

        int comparison = comparator(needle, haystack[middle]);

        if (comparison < 0)
            if (middle != 0)
                high = middle - 1;
            else
                break;
        else if (comparison > 0)
            low = middle + 1;
        else {
            if (nearby_index)
                *nearby_index = middle;
            return &haystack[middle];
        }
    }

    if (nearby_index)
        *nearby_index = min(low, high);

    return nullptr;
}

}

#if USING_AK_GLOBALLY
using AK::binary_search;
#endif

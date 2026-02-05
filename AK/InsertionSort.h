/*
 * Copyright (c) 2022, Marc Luqué <marc.luque@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/StdLibExtras.h>

namespace AK {

// Standard Insertion Sort, with `end` inclusive!
template<typename Collection, typename Comparator, typename T = decltype(declval<Collection>()[declval<int>()])>
void insertion_sort(Collection& col, ssize_t start, ssize_t end, Comparator comparator)
requires(Indexable<Collection, T>)
{
    for (ssize_t i = start + 1; i <= end; ++i) {
        for (ssize_t j = i; j > start && comparator(col[j], col[j - 1]); --j)
            swap(col[j], col[j - 1]);
    }
}

template<typename Collection, typename Comparator, typename T = decltype(declval<Collection>()[declval<int>()])>
void insertion_sort(Collection& collection, Comparator comparator)
requires(Indexable<Collection, T>)
{
    if (collection.size() == 0)
        return;
    insertion_sort(collection, 0, collection.size() - 1, move(comparator));
}

template<typename Collection, typename T = decltype(declval<Collection>()[declval<int>()])>
void insertion_sort(Collection& collection)
requires(Indexable<Collection, T>)
{
    if (collection.size() == 0)
        return;
    insertion_sort(collection, 0, collection.size() - 1, [](auto& a, auto& b) { return a < b; });
}

}

using AK::insertion_sort;

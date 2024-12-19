/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Marc Luqué <marc.luque@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/InsertionSort.h>
#include <AK/StdLibExtras.h>

namespace AK {

// This is a dual pivot quick sort. It is quite a bit faster than the single
// pivot quick_sort below. The other quick_sort below should only be used when
// you are stuck with simple iterators to a container and you don't have access
// to the container itself.
//
// We use a cutoff to insertion sort for partitions of size 7 or smaller.
// The idea is to avoid recursion for small partitions.
// The value 7 here is a magic number. According to princeton's CS algorithm class
// a value between 5 and 15 should work well in most situations:
// https://algs4.cs.princeton.edu/23quicksort/

static constexpr int INSERTION_SORT_CUTOFF = 7;

template<typename Collection, typename LessThan>
void dual_pivot_quick_sort(Collection& col, int start, int end, LessThan less_than)
{
    if ((end + 1) - start <= INSERTION_SORT_CUTOFF) {
        AK::insertion_sort(col, start, end, less_than);
        return;
    }

    while (start < end) {
        int size = end - start + 1;
        if (size > 3) {
            int third = size / 3;
            if (less_than(col[start + third], col[end - third])) {
                swap(col[start + third], col[start]);
                swap(col[end - third], col[end]);
            } else {
                swap(col[start + third], col[end]);
                swap(col[end - third], col[start]);
            }
        } else {
            if (!less_than(col[start], col[end])) {
                swap(col[start], col[end]);
            }
        }

        int j = start + 1;
        int k = start + 1;
        int g = end - 1;

        auto&& left_pivot = col[start];
        auto&& right_pivot = col[end];

        while (k <= g) {
            if (less_than(col[k], left_pivot)) {
                swap(col[k], col[j]);
                j++;
            } else if (!less_than(col[k], right_pivot)) {
                while (!less_than(col[g], right_pivot) && k < g) {
                    g--;
                }
                swap(col[k], col[g]);
                g--;
                if (less_than(col[k], left_pivot)) {
                    swap(col[k], col[j]);
                    j++;
                }
            }
            k++;
        }
        j--;
        g++;

        swap(col[start], col[j]);
        swap(col[end], col[g]);

        int left_pointer = j;
        int right_pointer = g;

        int left_size = left_pointer - start;
        int middle_size = right_pointer - (left_pointer + 1);
        int right_size = (end + 1) - (right_pointer + 1);

        if (left_size >= middle_size && left_size >= right_size) {
            dual_pivot_quick_sort(col, left_pointer + 1, right_pointer - 1, less_than);
            dual_pivot_quick_sort(col, right_pointer + 1, end, less_than);
            end = left_pointer - 1;
        } else if (middle_size >= right_size) {
            dual_pivot_quick_sort(col, start, left_pointer - 1, less_than);
            dual_pivot_quick_sort(col, right_pointer + 1, end, less_than);
            start = left_pointer + 1;
            end = right_pointer - 1;
        } else {
            dual_pivot_quick_sort(col, start, left_pointer - 1, less_than);
            dual_pivot_quick_sort(col, left_pointer + 1, right_pointer - 1, less_than);
            start = right_pointer + 1;
        }
    }
}

template<typename Iterator, typename LessThan>
void single_pivot_quick_sort(Iterator start, Iterator end, LessThan less_than)
{
    for (;;) {
        int size = end - start;
        if (size <= 1)
            return;

        int pivot_point = size / 2;
        swap(*(start + pivot_point), *start);

        auto&& pivot = *start;

        int i = 1;
        for (int j = 1; j < size; ++j) {
            if (less_than(*(start + j), pivot)) {
                swap(*(start + j), *(start + i));
                ++i;
            }
        }

        swap(*start, *(start + i - 1));
        // Recur into the shorter part of the remaining data
        // to ensure a stack depth of at most log(n).
        if (i > size / 2) {
            single_pivot_quick_sort(start + i, end, less_than);
            end = start + i - 1;
        } else {
            single_pivot_quick_sort(start, start + i - 1, less_than);
            start = start + i;
        }
    }
}

template<typename Iterator>
void quick_sort(Iterator start, Iterator end)
{
    single_pivot_quick_sort(start, end, [](auto& a, auto& b) { return a < b; });
}

template<typename Iterator, typename LessThan>
void quick_sort(Iterator start, Iterator end, LessThan less_than)
{
    single_pivot_quick_sort(start, end, move(less_than));
}

template<typename Collection, typename LessThan>
void quick_sort(Collection& collection, LessThan less_than)
{
    dual_pivot_quick_sort(collection, 0, collection.size() - 1, move(less_than));
}

template<typename Collection>
void quick_sort(Collection& collection)
{
    dual_pivot_quick_sort(collection, 0, collection.size() - 1,
        [](auto& a, auto& b) { return a < b; });
}

}

#if USING_AK_GLOBALLY
using AK::quick_sort;
#endif

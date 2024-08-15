/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/Random.h>
#include <AK/StdLibExtras.h>

namespace AK {

inline constexpr int MEDIAN_OF_MEDIAN_CUTOFF = 4500;

// FIXME: Stole and adapted these two functions from `Userland/Demos/Tubes/Tubes.cpp` we really need something like this in `AK/Random.h`
static inline double random_double()
{
    return get_random<u32>() / static_cast<double>(NumericLimits<u32>::max());
}

static inline size_t random_int(size_t min, size_t max)
{
    return min + round_to<size_t>(random_double() * (max - min));
}

// Implementations of common pivot functions
namespace PivotFunctions {

// Just use the first element of the range as the pivot
// Mainly used to debug the quick select algorithm
// Good with random data since it has nearly no overhead
// Attention: Turns the algorithm quadratic if used with already (partially) sorted data
template<typename Collection, typename LessThan>
size_t first_element([[maybe_unused]] Collection& collection, size_t left, [[maybe_unused]] size_t right, [[maybe_unused]] LessThan less_than)
{
    return left;
}

// Just use the middle element of the range as the pivot
// This is what is used in AK::single_pivot_quick_sort in quicksort.h
// Works fairly well with random Data
// Works incredibly well with sorted data since the pivot is always a perfect split
template<typename Collection, typename LessThan>
size_t middle_element([[maybe_unused]] Collection& collection, size_t left, size_t right, [[maybe_unused]] LessThan less_than)
{
    return (left + right) / 2;
}

// Pick a random Pivot
// This is the "Traditional" implementation of both quicksort and quick select
// Performs fairly well both with random and sorted data
template<typename Collection, typename LessThan>
size_t random_element([[maybe_unused]] Collection& collection, size_t left, size_t right, [[maybe_unused]] LessThan less_than)
{
    return random_int(left, right);
}

// Implementation detail of median_of_medians
// Whilst this looks quadratic in runtime, it always gets called with 5 or fewer elements so can be considered constant runtime
template<typename Collection, typename LessThan>
size_t partition5(Collection& collection, size_t left, size_t right, LessThan less_than)
{
    VERIFY((right - left) <= 5);
    for (size_t i = left + 1; i <= right; i++) {
        for (size_t j = i; j > left && less_than(collection.at(j), collection.at(j - 1)); j--) {
            swap(collection.at(j), collection.at(j - 1));
        }
    }
    return (left + right) / 2;
}

// https://en.wikipedia.org/wiki/Median_of_medians
// Use the median of medians algorithm to pick a really good pivot
// This makes quick select run in linear time but comes with a lot of overhead that only pays off with very large inputs
template<typename Collection, typename LessThan>
size_t median_of_medians(Collection& collection, size_t left, size_t right, LessThan less_than)
{
    if ((right - left) < 5)
        return partition5(collection, left, right, less_than);

    for (size_t i = left; i <= right; i += 5) {
        size_t sub_right = i + 4;
        if (sub_right > right)
            sub_right = right;

        size_t median5 = partition5(collection, i, sub_right, less_than);
        swap(collection.at(median5), collection.at(left + (i - left) / 5));
    }
    size_t mid = (right - left) / 10 + left + 1;

    // We're using mutual recursion here, using quickselect_inplace to find the pivot for quickselect_inplace.
    // Whilst this achieves True linear Runtime, it is a lot of overhead, so use only this variant with very large inputs
    return quickselect_inplace(
        collection, left, left + ((right - left) / 5), mid, [](auto collection, size_t left, size_t right, auto less_than) { return AK::PivotFunctions::median_of_medians(collection, left, right, less_than); }, less_than);
}

}

// This is the Lomuto Partition scheme which is simpler but less efficient than Hoare's partitioning scheme that is traditionally used with quicksort
// https://en.wikipedia.org/wiki/Quicksort#Lomuto_partition_scheme
template<typename Collection, typename PivotFn, typename LessThan>
static size_t partition(Collection& collection, size_t left, size_t right, PivotFn pivot_fn, LessThan less_than)
{
    auto pivot_index = pivot_fn(collection, left, right, less_than);
    auto pivot_value = collection.at(pivot_index);
    swap(collection.at(pivot_index), collection.at(right));
    auto store_index = left;

    for (size_t i = left; i < right; i++) {
        if (less_than(collection.at(i), pivot_value)) {
            swap(collection.at(store_index), collection.at(i));
            store_index++;
        }
    }

    swap(collection.at(right), collection.at(store_index));
    return store_index;
}

template<typename Collection, typename PivotFn, typename LessThan>
size_t quickselect_inplace(Collection& collection, size_t left, size_t right, size_t k, PivotFn pivot_fn, LessThan less_than)
{
    // Bail if left is somehow bigger than right and return default constructed result
    // FIXME: This can also occur when the collection is empty maybe propagate this error somehow?
    // returning 0 would be a really bad thing since this returns and index and that might lead to memory errors
    // returning in ErrorOr<size_t> here might be a good option but this is a very specific error that in nearly all circumstances should be considered a bug on the callers site
    VERIFY(left <= right);

    // If there's only one element, return that element
    if (left == right)
        return left;

    auto pivot_index = partition(collection, left, right, pivot_fn, less_than);

    // we found the thing we were searching for
    if (k == pivot_index)
        return k;

    // Recurse on the left side
    if (k < pivot_index)
        return quickselect_inplace(collection, left, pivot_index - 1, k, pivot_fn, less_than);

    // recurse on the right side
    return quickselect_inplace(collection, pivot_index + 1, right, k, pivot_fn, less_than);
}

//
template<typename Collection, typename PivotFn, typename LessThan>
size_t quickselect_inplace(Collection& collection, size_t k, PivotFn pivot_fn, LessThan less_than)
{
    return quickselect_inplace(collection, 0, collection.size() - 1, k, pivot_fn, less_than);
}

template<typename Collection, typename PivotFn>
size_t quickselect_inplace(Collection& collection, size_t k, PivotFn pivot_fn)
{
    return quickselect_inplace(collection, 0, collection.size() - 1, k, pivot_fn, [](auto& a, auto& b) { return a < b; });
}

// All of these quick select implementation versions return the `index` of the resulting element, after the algorithm has run, not the element itself!
// As Part of the Algorithm, they all modify the collection in place, partially sorting it in the process.
template<typename Collection>
size_t quickselect_inplace(Collection& collection, size_t k)
{
    if (collection.size() >= MEDIAN_OF_MEDIAN_CUTOFF)
        return quickselect_inplace(
            collection, 0, collection.size() - 1, k, [](auto collection, size_t left, size_t right, auto less_than) { return PivotFunctions::median_of_medians(collection, left, right, less_than); }, [](auto& a, auto& b) { return a < b; });

    else
        return quickselect_inplace(
            collection, 0, collection.size() - 1, k, [](auto collection, size_t left, size_t right, auto less_than) { return PivotFunctions::random_element(collection, left, right, less_than); }, [](auto& a, auto& b) { return a < b; });
}

}

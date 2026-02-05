/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Noncopyable.h>
#include <AK/QuickSort.h>
#include <AK/StdLibExtras.h>

TEST_CASE(sorts_without_copy)
{
    struct NoCopy {
        AK_MAKE_NONCOPYABLE(NoCopy);
        AK_MAKE_DEFAULT_MOVABLE(NoCopy);

    public:
        NoCopy() = default;

        int value { 0 };
    };

    Array<NoCopy, 64> array;

    // Test the dual pivot quick sort.
    for (size_t i = 0; i < 64; ++i)
        array[i].value = (64 - i) % 32 + 32;

    dual_pivot_quick_sort(array, 0, array.size() - 1, [](auto& a, auto& b) { return a.value < b.value; });

    for (size_t i = 0; i < 63; ++i)
        EXPECT(array[i].value <= array[i + 1].value);

    // Test the single pivot quick sort.
    for (size_t i = 0; i < 64; ++i)
        array[i].value = (64 - i) % 32 + 32;

    AK::single_pivot_quick_sort(array.begin(), array.end(), [](auto& a, auto& b) { return a.value < b.value; });

    for (size_t i = 0; i < 63; ++i)
        EXPECT(array[i].value <= array[i + 1].value);
}

// This test case may fail to construct a worst-case input if the pivot choice
// of the underlying quick_sort no longer matches the one used here.
// So it provides no strong guarantees about the properties of quick_sort.
TEST_CASE(maximum_stack_depth)
{
    int const size = 256;
    int* data = new int[size];

    for (int i = 0; i < size; i++) {
        data[i] = i;
    }

    // Construct the data in such a way that the assumed pivot choice
    // of (size / 2) causes the partitions to be of worst case size.
    for (int i = 0; i < size / 2; i++) {
        swap(data[i], data[i + (size - i) / 2]);
    }

    // Measure the depth of the call stack through the less_than argument
    // of quick_sort as it gets copied for each recursive call.
    struct DepthMeasurer {
        int& max_depth;
        int depth { 0 };
        DepthMeasurer(int& max_depth)
            : max_depth(max_depth)
        {
        }
        DepthMeasurer(DepthMeasurer const& obj)
            : max_depth(obj.max_depth)
        {
            depth = obj.depth + 1;
            if (depth > max_depth) {
                max_depth = depth;
            }
        }
        bool operator()(int& a, int& b)
        {
            return a < b;
        }
    };

    int max_depth = 0;
    DepthMeasurer measurer(max_depth);
    AK::single_pivot_quick_sort(data, data + size, measurer);

    EXPECT(max_depth <= 64);

    for (int i = 0; i < size; i++)
        EXPECT(data[i] == i);

    delete[] data;
}

/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/BinarySearch.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <cstring>
#include <new>

using namespace Test::Randomized;

TEST_CASE(vector_ints)
{
    Vector<int> ints;
    ints.append(1);
    ints.append(2);
    ints.append(3);

    auto test1 = *binary_search(ints, 1);
    auto test2 = *binary_search(ints, 2);
    auto test3 = *binary_search(ints, 3);
    EXPECT_EQ(test1, 1);
    EXPECT_EQ(test2, 2);
    EXPECT_EQ(test3, 3);
}

TEST_CASE(span_rvalue_reference)
{
    Array<long, 3> array { 1, 2, 3 };

    size_t nearby_index = 0;
    auto* pointer = binary_search(array.span(), 2, &nearby_index);

    EXPECT_EQ(nearby_index, 1u);
    EXPECT_EQ(pointer, &array[1]);
}

TEST_CASE(array_doubles)
{
    Array<double, 3> array { 1.1, 9.9, 33.33 };

    EXPECT_EQ(binary_search(array, 1.1), &array[0]);
    EXPECT_EQ(binary_search(array, 33.33), &array[2]);
    EXPECT_EQ(binary_search(array, 9.9), &array[1]);
}

TEST_CASE(vector_strings)
{
    Vector<ByteString> strings;
    strings.append("bat");
    strings.append("cat");
    strings.append("dog");

    auto string_compare = [](ByteString const& a, ByteString const& b) -> int {
        return strcmp(a.characters(), b.characters());
    };
    auto test1 = *binary_search(strings, ByteString("bat"), nullptr, string_compare);
    auto test2 = *binary_search(strings, ByteString("cat"), nullptr, string_compare);
    auto test3 = *binary_search(strings, ByteString("dog"), nullptr, string_compare);
    EXPECT_EQ(test1, ByteString("bat"));
    EXPECT_EQ(test2, ByteString("cat"));
    EXPECT_EQ(test3, ByteString("dog"));
}

TEST_CASE(single_element)
{
    Vector<int> ints;
    ints.append(1);

    auto test1 = *binary_search(ints, 1);
    EXPECT_EQ(test1, 1);
}

TEST_CASE(not_found)
{
    Vector<int> ints;
    ints.append(1);
    ints.append(2);
    ints.append(3);

    auto test1 = binary_search(ints, -1);
    auto test2 = binary_search(ints, 0);
    auto test3 = binary_search(ints, 4);
    EXPECT_EQ(test1, nullptr);
    EXPECT_EQ(test2, nullptr);
    EXPECT_EQ(test3, nullptr);
}

TEST_CASE(no_elements)
{
    Vector<int> ints;

    auto test1 = binary_search(ints, 1);
    EXPECT_EQ(test1, nullptr);
}

TEST_CASE(constexpr_array_search)
{
    constexpr Array<int, 3> array = { 1, 17, 42 };

    static_assert(binary_search(array, 42) == &array[2]);
    static_assert(binary_search(array, 17) == &array[1]);
    static_assert(binary_search(array, 3) == nullptr);
}

TEST_CASE(unsigned_to_signed_regression)
{
    Array<u32, 5> const input { 0, 1, 2, 3, 4 };

    // The algorithm computes 1 - input[2] = -1, and if this is (incorrectly) cast
    // to an unsigned then it will look in the wrong direction and miss the 1.

    size_t nearby_index = 1;
    EXPECT_EQ(binary_search(input, 1u, &nearby_index), &input[1]);
    EXPECT_EQ(nearby_index, 1u);
}

RANDOMIZED_TEST_CASE(finds_number_that_is_present)
{
    GEN(vec, Gen::vector(1, 16, []() { return Gen::number_u64(); }));
    GEN(i, Gen::number_u64(0, vec.size() - 1));
    AK::quick_sort(vec);
    u64 n = vec[i];
    auto ptr = binary_search(vec, n);
    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(*ptr, n);
}

RANDOMIZED_TEST_CASE(doesnt_find_number_that_is_not_present)
{
    GEN(vec, Gen::vector(1, 16, []() { return Gen::number_u64(); }));
    AK::quick_sort(vec);

    u64 not_present = 0;
    while (!vec.find(not_present).is_end()) {
        ++not_present;
    }

    EXPECT_EQ(binary_search(vec, not_present), nullptr);
}

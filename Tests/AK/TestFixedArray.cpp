/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibTest/TestSuite.h>

#include <AK/FixedArray.h>
#include <AK/NoAllocationGuard.h>

TEST_CASE(construct)
{
    EXPECT_EQ(FixedArray<int>().size(), 0u);
    EXPECT_EQ(FixedArray<int>::must_create_but_fixme_should_propagate_errors(1985).size(), 1985u);
}

TEST_CASE(ints)
{
    FixedArray<int> ints = FixedArray<int>::must_create_but_fixme_should_propagate_errors(3);
    ints[0] = 0;
    ints[1] = 1;
    ints[2] = 2;
    EXPECT_EQ(ints[0], 0);
    EXPECT_EQ(ints[1], 1);
    EXPECT_EQ(ints[2], 2);
}

TEST_CASE(conforms_to_iterator_procotol)
{
    static_assert(std::random_access_iterator<FixedArray<int>::Iterator>);
    static_assert(std::random_access_iterator<FixedArray<int>::ConstIterator>);
    static_assert(std::random_access_iterator<FixedArray<int const>::Iterator>);
    static_assert(std::random_access_iterator<FixedArray<int const>::ConstIterator>);
}

TEST_CASE(swap)
{
    FixedArray<int> first = FixedArray<int>::must_create_but_fixme_should_propagate_errors(4);
    FixedArray<int> second = FixedArray<int>::must_create_but_fixme_should_propagate_errors(5);
    first[3] = 1;
    second[3] = 2;
    first.swap(second);
    EXPECT_EQ(first.size(), 5u);
    EXPECT_EQ(second.size(), 4u);
    EXPECT_EQ(first[3], 2);
    EXPECT_EQ(second[3], 1);
}

TEST_CASE(move)
{
    FixedArray<int> moved_from_array = FixedArray<int>::must_create_but_fixme_should_propagate_errors(6);
    FixedArray<int> moved_to_array(move(moved_from_array));
    EXPECT_EQ(moved_to_array.size(), 6u);
    EXPECT_EQ(moved_from_array.size(), 0u);
}

TEST_CASE(no_allocation)
{
    FixedArray<int> array = FixedArray<int>::must_create_but_fixme_should_propagate_errors(5);
    EXPECT_NO_CRASH("Assignments", [&] {
        NoAllocationGuard guard;
        array[0] = 0;
        array[1] = 1;
        array[2] = 2;
        array[4] = array[1];
        array[3] = array[0] + array[2];
        return Test::Crash::Failure::DidNotCrash;
    });

    EXPECT_NO_CRASH("Move", [&] {
        FixedArray<int> moved_from_array = FixedArray<int>::must_create_but_fixme_should_propagate_errors(6);
        // We need an Optional here to ensure that the NoAllocationGuard is
        // destroyed before the moved_to_array, because that would call free
        Optional<FixedArray<int>> moved_to_array;

        {
            NoAllocationGuard guard;
            moved_to_array.emplace(move(moved_from_array));
        }

        return Test::Crash::Failure::DidNotCrash;
    });

    EXPECT_NO_CRASH("Swap", [&] {
        FixedArray<int> target_for_swapping;
        {
            NoAllocationGuard guard;
            array.swap(target_for_swapping);
        }
        return Test::Crash::Failure::DidNotCrash;
    });
}

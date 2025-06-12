/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/TypedTransfer.h>

struct NonPrimitiveIntWrapper {
    NonPrimitiveIntWrapper(int value)
        : m_value(value)
    {
    }

    int m_value;
};

TEST_CASE(overlapping_source_and_destination_1)
{
    Array<NonPrimitiveIntWrapper, 6> const expected { 3, 4, 5, 6, 5, 6 };

    Array<NonPrimitiveIntWrapper, 6> actual { 1, 2, 3, 4, 5, 6 };
    AK::TypedTransfer<NonPrimitiveIntWrapper>::copy(actual.data(), actual.data() + 2, 4);

    for (size_t i = 0; i < 6; ++i)
        EXPECT_EQ(actual[i].m_value, expected[i].m_value);
}

TEST_CASE(overlapping_source_and_destination_2)
{
    Array<NonPrimitiveIntWrapper, 6> const expected { 1, 2, 1, 2, 3, 4 };

    Array<NonPrimitiveIntWrapper, 6> actual { 1, 2, 3, 4, 5, 6 };
    AK::TypedTransfer<NonPrimitiveIntWrapper>::copy(actual.data() + 2, actual.data(), 4);

    for (size_t i = 0; i < 6; ++i)
        EXPECT_EQ(actual[i].m_value, expected[i].m_value);
}

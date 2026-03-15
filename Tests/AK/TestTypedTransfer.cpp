/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/ScopeGuard.h>
#include <AK/TypedTransfer.h>

struct NonPrimitiveIntWrapper {
    NonPrimitiveIntWrapper(int value)
        : m_value(value)
    {
    }

    int m_value;
};

static_assert(IsTriviallyRelocatable<int>);

struct NonTriviallyRelocatable {
    NonTriviallyRelocatable(int value)
        : m_value(value)
    {
        ++s_construct_count;
    }

    NonTriviallyRelocatable(NonTriviallyRelocatable&& other)
        : m_value(other.m_value)
    {
        other.m_value = -1;
        ++s_construct_count;
    }

    ~NonTriviallyRelocatable()
    {
        ++s_destruct_count;
    }

    NonTriviallyRelocatable(NonTriviallyRelocatable const&) = delete;
    NonTriviallyRelocatable& operator=(NonTriviallyRelocatable const&) = delete;

    int m_value;

    static int s_construct_count;
    static int s_destruct_count;
};

int NonTriviallyRelocatable::s_construct_count = 0;
int NonTriviallyRelocatable::s_destruct_count = 0;

static_assert(!IsTriviallyRelocatable<NonTriviallyRelocatable>);

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

TEST_CASE(relocate_trivially_relocatable)
{
    Array<int, 4> source { 10, 20, 30, 40 };
    alignas(int) u8 destination_storage[4 * sizeof(int)];
    auto* destination = reinterpret_cast<int*>(destination_storage);

    AK::TypedTransfer<int>::relocate(destination, source.data(), 4);

    EXPECT_EQ(destination[0], 10);
    EXPECT_EQ(destination[1], 20);
    EXPECT_EQ(destination[2], 30);
    EXPECT_EQ(destination[3], 40);
}

TEST_CASE(relocate_non_trivially_relocatable)
{
    alignas(NonTriviallyRelocatable) u8 source_storage[3 * sizeof(NonTriviallyRelocatable)];
    alignas(NonTriviallyRelocatable) u8 destination_storage[3 * sizeof(NonTriviallyRelocatable)];
    auto* source = reinterpret_cast<NonTriviallyRelocatable*>(source_storage);
    auto* destination = reinterpret_cast<NonTriviallyRelocatable*>(destination_storage);

    new (&source[0]) NonTriviallyRelocatable(100);
    new (&source[1]) NonTriviallyRelocatable(200);
    new (&source[2]) NonTriviallyRelocatable(300);

    ScopeGuard cleanup([&] {
        AK::TypedTransfer<NonTriviallyRelocatable>::delete_(destination, 3);
    });

    NonTriviallyRelocatable::s_construct_count = 0;
    NonTriviallyRelocatable::s_destruct_count = 0;

    AK::TypedTransfer<NonTriviallyRelocatable>::relocate(destination, source, 3);

    EXPECT_EQ(destination[0].m_value, 100);
    EXPECT_EQ(destination[1].m_value, 200);
    EXPECT_EQ(destination[2].m_value, 300);

    EXPECT_EQ(NonTriviallyRelocatable::s_construct_count, 3);
    EXPECT_EQ(NonTriviallyRelocatable::s_destruct_count, 3);
}

TEST_CASE(relocate_zero_count)
{
    NonTriviallyRelocatable::s_construct_count = 0;
    NonTriviallyRelocatable::s_destruct_count = 0;

    AK::TypedTransfer<NonTriviallyRelocatable>::relocate(nullptr, nullptr, 0);

    EXPECT_EQ(NonTriviallyRelocatable::s_construct_count, 0);
    EXPECT_EQ(NonTriviallyRelocatable::s_destruct_count, 0);
}

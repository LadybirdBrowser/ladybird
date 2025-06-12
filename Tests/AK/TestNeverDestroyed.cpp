/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/NeverDestroyed.h>
#include <AK/StdLibExtras.h>

struct Counter {
    Counter() = default;

    ~Counter() { ++num_destroys; }

    Counter(Counter const&)
    {
        ++num_copies;
    }

    Counter(Counter&&) { ++num_moves; }

    int num_copies {};
    int num_moves {};
    int num_destroys {};
};

TEST_CASE(should_construct_by_copy)
{
    Counter c {};
    AK::NeverDestroyed<Counter> n { c };

    EXPECT_EQ(1, n->num_copies);
    EXPECT_EQ(0, n->num_moves);
}

TEST_CASE(should_construct_by_move)
{
    Counter c {};
    AK::NeverDestroyed<Counter> n { move(c) };

    EXPECT_EQ(0, n->num_copies);
    EXPECT_EQ(1, n->num_moves);
}

struct DestructorChecker {
    DestructorChecker(bool& destroyed)
        : m_destroyed(destroyed)
    {
    }

    ~DestructorChecker()
    {
        m_destroyed = true;
    }

    bool& m_destroyed;
};

TEST_CASE(should_not_destroy)
{
    bool destroyed = false;
    {
        AK::NeverDestroyed<DestructorChecker> n(destroyed);
    }
    EXPECT(!destroyed);
}

TEST_CASE(should_provide_dereference_operator)
{
    AK::NeverDestroyed<Counter> n {};
    EXPECT_EQ(0, n->num_destroys);
}

TEST_CASE(should_provide_indirection_operator)
{
    AK::NeverDestroyed<Counter> n {};
    EXPECT_EQ(0, (*n).num_destroys);
}

TEST_CASE(should_provide_basic_getter)
{
    AK::NeverDestroyed<Counter> n {};
    EXPECT_EQ(0, n.get().num_destroys);
}

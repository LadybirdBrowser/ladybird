/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/CircularQueue.h>

TEST_CASE(basic)
{
    CircularQueue<int, 3> ints;
    EXPECT(ints.is_empty());
    ints.enqueue(1);
    ints.enqueue(2);
    ints.enqueue(3);
    EXPECT_EQ(ints.size(), 3u);

    ints.enqueue(4);
    EXPECT_EQ(ints.size(), 3u);
    EXPECT_EQ(ints.dequeue(), 2);
    EXPECT_EQ(ints.dequeue(), 3);
    EXPECT_EQ(ints.dequeue(), 4);
    EXPECT_EQ(ints.size(), 0u);
}

TEST_CASE(complex_type)
{
    CircularQueue<ByteString, 2> strings;

    strings.enqueue("ABC");
    strings.enqueue("DEF");

    EXPECT_EQ(strings.size(), 2u);

    strings.enqueue("abc");
    strings.enqueue("def");

    EXPECT_EQ(strings.dequeue(), "abc");
    EXPECT_EQ(strings.dequeue(), "def");
}

TEST_CASE(complex_type_clear)
{
    CircularQueue<ByteString, 5> strings;
    strings.enqueue("xxx");
    strings.enqueue("xxx");
    strings.enqueue("xxx");
    strings.enqueue("xxx");
    strings.enqueue("xxx");
    EXPECT_EQ(strings.size(), 5u);
    strings.clear();
    EXPECT_EQ(strings.size(), 0u);
}

struct ConstructorCounter {
    static unsigned s_num_constructor_calls;
    ConstructorCounter() { ++s_num_constructor_calls; }
};
unsigned ConstructorCounter::s_num_constructor_calls = 0;

TEST_CASE(should_not_call_value_type_constructor_when_created)
{
    CircularQueue<ConstructorCounter, 10> queue;
    EXPECT_EQ(0u, ConstructorCounter::s_num_constructor_calls);
}

/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/Queue.h>

TEST_CASE(construct)
{
    EXPECT(Queue<int>().is_empty());
    EXPECT(Queue<int>().size() == 0);
}

TEST_CASE(populate_int)
{
    Queue<int> ints;
    ints.enqueue(1);
    ints.enqueue(2);
    ints.enqueue(3);
    EXPECT_EQ(ints.size(), 3u);
    EXPECT_EQ(ints.dequeue(), 1);
    EXPECT_EQ(ints.size(), 2u);
    EXPECT_EQ(ints.dequeue(), 2);
    EXPECT_EQ(ints.size(), 1u);
    EXPECT_EQ(ints.dequeue(), 3);
    EXPECT_EQ(ints.size(), 0u);
}

TEST_CASE(populate_string)
{
    Queue<ByteString> strings;
    strings.enqueue("ABC");
    strings.enqueue("DEF");
    EXPECT_EQ(strings.size(), 2u);
    EXPECT_EQ(strings.dequeue(), "ABC");
    EXPECT_EQ(strings.dequeue(), "DEF");
    EXPECT(strings.is_empty());
}

TEST_CASE(order)
{
    Queue<ByteString> strings;
    EXPECT(strings.is_empty());

    for (size_t i = 0; i < 10000; ++i) {
        strings.enqueue(ByteString::number(i));
        EXPECT_EQ(strings.size(), i + 1);
    }

    for (int i = 0; i < 10000; ++i) {
        EXPECT_EQ(strings.dequeue().to_number<int>().value(), i);
    }

    EXPECT(strings.is_empty());
}

/*
 * Copyright (c) 2021, Jesse Buhagiar <jooster669@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Stack.h>

TEST_CASE(basic)
{
    AK::Stack<int, 3> stack;

    EXPECT(stack.is_empty() == true);
    stack.push(2);
    stack.push(4);
    stack.push(17);
    EXPECT(stack.size() == 3);
    EXPECT(stack.top() == 17);
    EXPECT_EQ(stack.pop(), true);
    EXPECT_EQ(stack.pop(), true);
    EXPECT_EQ(stack.pop(), true);
    EXPECT(stack.is_empty());
}

TEST_CASE(complex_type)
{
    AK::Stack<ByteString, 4> stack;

    EXPECT_EQ(stack.is_empty(), true);
    EXPECT(stack.push("Well"));
    EXPECT(stack.push("Hello"));
    EXPECT(stack.push("Friends"));
    EXPECT(stack.push(":^)"));
    EXPECT_EQ(stack.top(), ":^)");
    EXPECT_EQ(stack.pop(), true);
    EXPECT_EQ(stack.top(), "Friends");
    EXPECT_EQ(stack.pop(), true);
    EXPECT_EQ(stack.top(), "Hello");
    EXPECT_EQ(stack.pop(), true);
    EXPECT_EQ(stack.top(), "Well");
    EXPECT_EQ(stack.pop(), true);
    EXPECT(stack.is_empty());
}

/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenity.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestSuite.h>

#include <AK/Tuple.h>

TEST_CASE(basic)
{
    Tuple<int, ByteString> value { 1, "foo" };
    EXPECT_EQ(value.get<int>(), 1);
    EXPECT_EQ(value.get<ByteString>(), "foo");
    EXPECT_EQ(value.get<0>(), 1);
    EXPECT_EQ(value.get<1>(), "foo");

    // Move assignment
    value = { 2, "bar" };
    EXPECT_EQ(value.get<int>(), 2);
    EXPECT_EQ(value.get<ByteString>(), "bar");
    EXPECT_EQ(value.get<0>(), 2);
    EXPECT_EQ(value.get<1>(), "bar");

    // Copy ctor
    auto other_value { value };
    EXPECT_EQ(other_value.get<int>(), 2);
    EXPECT_EQ(other_value.get<ByteString>(), "bar");
    EXPECT_EQ(other_value.get<0>(), 2);
    EXPECT_EQ(other_value.get<1>(), "bar");

    // Move ctor
    auto moved_to_value { move(value) };
    EXPECT_EQ(moved_to_value.get<int>(), 2);
    EXPECT_EQ(moved_to_value.get<ByteString>(), "bar");
    EXPECT_EQ(moved_to_value.get<0>(), 2);
    EXPECT_EQ(moved_to_value.get<1>(), "bar");

    // Copy assignment
    value = moved_to_value;
    EXPECT_EQ(moved_to_value.get<int>(), 2);
    EXPECT_EQ(moved_to_value.get<ByteString>(), "bar");
    EXPECT_EQ(moved_to_value.get<0>(), 2);
    EXPECT_EQ(moved_to_value.get<1>(), "bar");
    EXPECT_EQ(value.get<int>(), 2);
    EXPECT_EQ(value.get<ByteString>(), "bar");
    EXPECT_EQ(value.get<0>(), 2);
    EXPECT_EQ(value.get<1>(), "bar");
}

TEST_CASE(no_copy)
{
    struct NoCopy {
        AK_MAKE_NONCOPYABLE(NoCopy);
        AK_MAKE_DEFAULT_MOVABLE(NoCopy);

    public:
        NoCopy() = default;
    };

    // Deleted copy ctor should not cause an issue so long as the value isn't copied.
    Tuple<NoCopy, int, int> value { {}, 1, 2 };
    auto foo = move(value);
    EXPECT_EQ(foo.get<1>(), 1);
    EXPECT_EQ(foo.get<2>(), 2);
}

TEST_CASE(apply)
{
    Tuple<int, int, ByteString> args { 1, 2, "foo" };

    // With copy
    {
        bool was_called = false;
        args.apply_as_args([&](int a, int b, ByteString c) {
            was_called = true;
            EXPECT_EQ(a, 1);
            EXPECT_EQ(b, 2);
            EXPECT_EQ(c, "foo");
        });
        EXPECT(was_called);
    }

    // With reference
    {
        bool was_called = false;
        args.apply_as_args([&](int& a, int& b, ByteString& c) {
            was_called = true;
            EXPECT_EQ(a, 1);
            EXPECT_EQ(b, 2);
            EXPECT_EQ(c, "foo");
        });
        EXPECT(was_called);
    }

    // With const reference, taken from a const tuple
    {
        bool was_called = false;
        auto const& args_ref = args;
        args_ref.apply_as_args([&](int const& a, int const& b, ByteString const& c) {
            was_called = true;
            EXPECT_EQ(a, 1);
            EXPECT_EQ(b, 2);
            EXPECT_EQ(c, "foo");
        });
        EXPECT(was_called);
    }
}

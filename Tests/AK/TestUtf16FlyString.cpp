/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Utf16FlyString.h>

TEST_CASE(empty_string)
{
    Utf16FlyString fly {};
    EXPECT(fly.is_empty());
    EXPECT_EQ(fly, ""sv);

    // Short strings do not get stored in the fly string table.
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 0u);
}

TEST_CASE(short_string)
{
    Utf16FlyString fly1 { "foo"_utf16 };
    EXPECT_EQ(fly1, "foo"sv);

    Utf16FlyString fly2 { "foo"_utf16 };
    EXPECT_EQ(fly2, "foo"sv);

    Utf16FlyString fly3 { "bar"_utf16 };
    EXPECT_EQ(fly3, "bar"sv);

    EXPECT_EQ(fly1, fly2);
    EXPECT_NE(fly1, fly3);
    EXPECT_NE(fly2, fly3);

    EXPECT(fly1.to_utf16_string().has_short_ascii_storage());
    EXPECT(fly2.to_utf16_string().has_short_ascii_storage());
    EXPECT(fly3.to_utf16_string().has_short_ascii_storage());

    // Short strings do not get stored in the fly string table.
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 0u);
}

TEST_CASE(long_string)
{
    Utf16FlyString fly1 { "thisisdefinitelymorethan7bytes"_utf16 };
    EXPECT_EQ(fly1, "thisisdefinitelymorethan7bytes"sv);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);

    Utf16FlyString fly2 { "thisisdefinitelymorethan7bytes"_utf16 };
    EXPECT_EQ(fly2, "thisisdefinitelymorethan7bytes"sv);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);

    Utf16FlyString fly3 { "thisisalsoforsuremorethan7bytes"_utf16 };
    EXPECT_EQ(fly3, "thisisalsoforsuremorethan7bytes"sv);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 2u);

    EXPECT_EQ(fly1, fly2);
    EXPECT_NE(fly1, fly3);
    EXPECT_NE(fly2, fly3);

    EXPECT(fly1.to_utf16_string().has_long_ascii_storage());
    EXPECT(fly2.to_utf16_string().has_long_ascii_storage());
    EXPECT(fly3.to_utf16_string().has_long_ascii_storage());
}

TEST_CASE(user_defined_literal)
{
    auto fly1 = "thisisdefinitelymorethan7bytes"_utf16_fly_string;
    EXPECT_EQ(fly1, "thisisdefinitelymorethan7bytes"sv);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);

    auto fly2 = "thisisdefinitelymorethan7bytes"_utf16_fly_string;
    EXPECT_EQ(fly2, "thisisdefinitelymorethan7bytes"sv);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);

    auto fly3 = u"thisisdefinitelymorethan7bytes"_utf16_fly_string;
    EXPECT_EQ(fly3, u"thisisdefinitelymorethan7bytes"sv);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);

    auto fly4 = "foo"_utf16_fly_string;
    EXPECT_EQ(fly4, "foo"sv);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);

    EXPECT_EQ(fly1, fly2);
    EXPECT_EQ(fly1, fly3);
    EXPECT_EQ(fly3, fly3);

    EXPECT_NE(fly1, fly4);
    EXPECT_NE(fly2, fly4);
    EXPECT_NE(fly3, fly4);
}

TEST_CASE(fly_string_keep_string_data_alive)
{
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 0u);
    {
        Utf16FlyString fly {};
        {
            auto string = "thisisdefinitelymorethan7bytes"_utf16;
            fly = Utf16FlyString { string };
            EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);
        }

        EXPECT_EQ(fly, "thisisdefinitelymorethan7bytes"sv);
        EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);
    }

    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 0u);
}

TEST_CASE(moved_fly_string_becomes_empty)
{
    Utf16FlyString fly1 {};
    EXPECT(fly1.is_empty());

    Utf16FlyString fly2 { "thisisdefinitelymorethan7bytes"_utf16 };
    EXPECT_EQ(fly2, "thisisdefinitelymorethan7bytes"sv);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);

    fly1 = move(fly2);

    EXPECT(fly2.is_empty());
    EXPECT_EQ(fly1, "thisisdefinitelymorethan7bytes"sv);
    EXPECT_EQ(Utf16FlyString::number_of_utf16_fly_strings(), 1u);
}

TEST_CASE(is_one_of)
{
    auto foo = Utf16FlyString::from_utf8("foo"sv);
    auto bar = Utf16FlyString::from_utf16(u"bar"sv);

    EXPECT(foo.is_one_of(foo));
    EXPECT(foo.is_one_of(foo, bar));
    EXPECT(foo.is_one_of(bar, foo));
    EXPECT(!foo.is_one_of(bar));

    EXPECT(!bar.is_one_of("foo"sv));
    EXPECT(bar.is_one_of("foo"sv, "bar"sv));
    EXPECT(bar.is_one_of("bar"sv, "foo"sv));
    EXPECT(bar.is_one_of("bar"sv));
}

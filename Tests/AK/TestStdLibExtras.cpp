/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestSuite.h>

#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>

TEST_CASE(ceil_div)
{
    EXPECT_EQ(ceil_div(0, 1), 0);
    EXPECT_EQ(ceil_div(1, 1), 1);
    EXPECT_EQ(ceil_div(2, 1), 2);
    EXPECT_EQ(ceil_div(3, 1), 3);
    EXPECT_EQ(ceil_div(4, 1), 4);

    EXPECT_EQ(ceil_div(-0, 1), 0);
    EXPECT_EQ(ceil_div(-1, 1), -1);
    EXPECT_EQ(ceil_div(-2, 1), -2);
    EXPECT_EQ(ceil_div(-3, 1), -3);
    EXPECT_EQ(ceil_div(-4, 1), -4);

    EXPECT_EQ(ceil_div(0, -1), 0);
    EXPECT_EQ(ceil_div(1, -1), -1);
    EXPECT_EQ(ceil_div(2, -1), -2);
    EXPECT_EQ(ceil_div(3, -1), -3);
    EXPECT_EQ(ceil_div(4, -1), -4);

    EXPECT_EQ(ceil_div(-0, -1), 0);
    EXPECT_EQ(ceil_div(-1, -1), 1);
    EXPECT_EQ(ceil_div(-2, -1), 2);
    EXPECT_EQ(ceil_div(-3, -1), 3);
    EXPECT_EQ(ceil_div(-4, -1), 4);

    EXPECT_EQ(ceil_div(0, 2), 0);
    EXPECT_EQ(ceil_div(1, 2), 1);
    EXPECT_EQ(ceil_div(2, 2), 1);
    EXPECT_EQ(ceil_div(3, 2), 2);
    EXPECT_EQ(ceil_div(4, 2), 2);

    EXPECT_EQ(ceil_div(-0, 2), 0);
    EXPECT_EQ(ceil_div(-1, 2), 0);
    EXPECT_EQ(ceil_div(-2, 2), -1);
    EXPECT_EQ(ceil_div(-3, 2), -1);
    EXPECT_EQ(ceil_div(-4, 2), -2);

    EXPECT_EQ(ceil_div(0, -2), 0);
    EXPECT_EQ(ceil_div(1, -2), 0);
    EXPECT_EQ(ceil_div(2, -2), -1);
    EXPECT_EQ(ceil_div(3, -2), -1);
    EXPECT_EQ(ceil_div(4, -2), -2);

    EXPECT_EQ(ceil_div(-0, -2), 0);
    EXPECT_EQ(ceil_div(-1, -2), 1);
    EXPECT_EQ(ceil_div(-2, -2), 1);
    EXPECT_EQ(ceil_div(-3, -2), 2);
    EXPECT_EQ(ceil_div(-4, -2), 2);
}

TEST_CASE(mix)
{
    double a = 1.0;
    double b = 3.0;

    EXPECT_APPROXIMATE(mix(a, b, 0.0), 1.0);
    EXPECT_APPROXIMATE(mix(a, b, 0.5), 2.0);
    EXPECT_APPROXIMATE(mix(a, b, 1.0), 3.0);

    EXPECT_APPROXIMATE(mix(b, a, 0.0), 3.0);
    EXPECT_APPROXIMATE(mix(b, a, 0.5), 2.0);
    EXPECT_APPROXIMATE(mix(b, a, 1.0), 1.0);
}

TEST_CASE(swap)
{
    int i = 4;
    int j = 6;

    swap(i, j);

    EXPECT_EQ(i, 6);
    EXPECT_EQ(j, 4);
}

TEST_CASE(swap_same_value)
{

    int i = 4;
    swap(i, i);
    EXPECT_EQ(i, 4);
}

TEST_CASE(swap_same_complex_object)
{
    struct Type1 {
        StringView foo;
    };
    struct Type2 {
        Optional<Type1> foo;
        Vector<Type1> bar;
    };

    Variant<Type1, Type2> value1 { Type1 { "hello"sv } };
    Variant<Type1, Type2> value2 { Type2 { {}, { { "goodbye"sv } } } };

    swap(value1, value2);

    EXPECT(value1.has<Type2>());
    EXPECT(value2.has<Type1>());

    swap(value1, value1);

    EXPECT(value1.has<Type2>());
    EXPECT(value2.has<Type1>());
}

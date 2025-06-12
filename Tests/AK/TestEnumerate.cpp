/*
 * Copyright (c) 2024, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Enumerate.h>
#include <AK/Span.h>
#include <AK/Vector.h>

struct IndexAndValue {
    size_t index;
    int value;

    bool operator==(IndexAndValue const&) const = default;
};

TEST_CASE(enumerate)
{
    {
        Vector<IndexAndValue> result;
        for (auto [i, value] : enumerate(Vector { 1, 2, 3, 4 })) {
            result.append({ i, value });
        }
        EXPECT_EQ(result, (Vector<IndexAndValue> { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 } }));
    }

    {
        Vector<IndexAndValue> result;
        Vector<int> values = { 9, 8, 7, 6 };
        for (auto [i, value] : enumerate(values)) {
            static_assert(SameAs<decltype(value), int&>);
            result.append({ i, value });
            value = static_cast<int>(i);
        }
        EXPECT_EQ(result, (Vector<IndexAndValue> { { 0, 9 }, { 1, 8 }, { 2, 7 }, { 3, 6 } }));
        EXPECT_EQ(values, (Vector<int> { 0, 1, 2, 3 }));
    }

    {
        Vector<IndexAndValue> result;
        Vector<int> const& values = { 9, 8, 7, 6 };
        for (auto [i, value] : enumerate(values)) {
            static_assert(SameAs<decltype(value), int const&>);
            result.append({ i, value });
        }
        EXPECT_EQ(result, (Vector<IndexAndValue> { { 0, 9 }, { 1, 8 }, { 2, 7 }, { 3, 6 } }));
    }
}

/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>

namespace {

struct CopyOnly {
    int& instance_count;

    CopyOnly(int& count)
        : instance_count(count)
    {
        instance_count++;
    }

    CopyOnly(CopyOnly const& other)
        : instance_count(other.instance_count)
    {
        instance_count++;
    }

    ~CopyOnly()
    {
        instance_count--;
    }
};

}

TEST_CASE(move_construction_destroys_old_inline_wrapper)
{
    int instance_count = 0;

    {
        Function<void()> source = [captured = CopyOnly(instance_count)]() {
            (void)captured;
        };
        EXPECT_EQ(instance_count, 1);

        Function<void()> destination = move(source);
        EXPECT_EQ(instance_count, 1);

        source = nullptr;
        EXPECT_EQ(instance_count, 1);
    }

    EXPECT_EQ(instance_count, 0);
}

TEST_CASE(move_assignment_destroys_old_inline_wrapper)
{
    int instance_count = 0;

    {
        Function<void()> source = [captured = CopyOnly(instance_count)]() {
            (void)captured;
        };
        EXPECT_EQ(instance_count, 1);

        Function<void()> destination;
        destination = move(source);
        EXPECT_EQ(instance_count, 1);

        source = nullptr;
        EXPECT_EQ(instance_count, 1);
    }

    EXPECT_EQ(instance_count, 0);
}

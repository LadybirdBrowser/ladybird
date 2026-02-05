/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/SegmentedVector.h>
#include <LibTest/TestCase.h>

TEST_CASE(append)
{
    AK::SegmentedVector<int, 2> segmented_vector;
    segmented_vector.append(1);
    segmented_vector.append(2);
    segmented_vector.append(3);
    EXPECT_EQ(segmented_vector.size(), 3u);
}

TEST_CASE(at)
{
    AK::SegmentedVector<int, 2> segmented_vector;
    segmented_vector.append(1);
    segmented_vector.append(2);
    segmented_vector.append(3);
    EXPECT_EQ(segmented_vector[0], 1);
    EXPECT_EQ(segmented_vector[1], 2);
    EXPECT_EQ(segmented_vector[2], 3);
}

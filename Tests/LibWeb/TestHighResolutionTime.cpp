/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

TEST_CASE(coarsening_time_is_idempotent)
{
    auto coarsened_time = Web::HighResolutionTime::coarsen_time(4'320'001'000.2001);
    EXPECT_EQ(coarsened_time, 4'320'001'000.2);
    EXPECT_EQ(Web::HighResolutionTime::coarsen_time(coarsened_time), coarsened_time);

    auto finely_coarsened_time = Web::HighResolutionTime::coarsen_time(4'320'001'000.0251, Web::HTML::CanUseCrossOriginIsolatedAPIs::Yes);
    EXPECT_EQ(finely_coarsened_time, nextafter(4'320'001'000.025, AK::Infinity<double>));
    EXPECT_EQ(Web::HighResolutionTime::coarsen_time(finely_coarsened_time, Web::HTML::CanUseCrossOriginIsolatedAPIs::Yes), finely_coarsened_time);
}

TEST_CASE(coarsening_negative_time_rounds_down)
{
    EXPECT_EQ(Web::HighResolutionTime::coarsen_time(-1.234), -1.3);
    EXPECT_EQ(Web::HighResolutionTime::coarsen_time(-1.234, Web::HTML::CanUseCrossOriginIsolatedAPIs::Yes), nextafter(-1.235, AK::Infinity<double>));
}

TEST_CASE(coarsening_time_does_not_cross_bucket_boundaries)
{
    EXPECT_EQ(Web::HighResolutionTime::coarsen_time(0.1), 0.1);
    EXPECT_EQ(Web::HighResolutionTime::coarsen_time(nextafter(0.1, -AK::Infinity<double>)), 0.0);
    EXPECT_EQ(Web::HighResolutionTime::coarsen_time(-0.1), -0.1);
    EXPECT_EQ(Web::HighResolutionTime::coarsen_time(nextafter(-0.1, -AK::Infinity<double>)), -0.2);
}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "test-web/InitialLoadTracker.h"
#include <LibTest/TestCase.h>

TEST_CASE(load_then_crash_does_not_count_a_view_twice)
{
    InitialLoadTracker tracker { 2 };
    tracker.mark_ready(0); // First view loads.
    EXPECT_EQ(tracker.ready_count(), 1uz);
    EXPECT(!tracker.all_ready());
    tracker.mark_ready(0); // The same view crashes while the second is pending.
    EXPECT_EQ(tracker.ready_count(), 1uz);
    EXPECT(!tracker.all_ready());
    tracker.mark_ready(1); // Second view crashes before loading.
    EXPECT(tracker.all_ready());
    tracker.mark_ready(1); // Its replacement finishes loading.
    EXPECT_EQ(tracker.ready_count(), 2uz);
}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/IdleCollectionPolicy.h>
#include <LibTest/TestCase.h>

using Decision = GC::IdleCollectionPolicy::Decision;

TEST_CASE(collects_once_the_allocation_rate_drops)
{
    GC::IdleCollectionPolicy policy;
    policy.reset(0);

    // A burst establishes the episode's peak rate; a single tick is never itself a rate drop.
    EXPECT(policy.evaluate(16 * MiB, 16 * MiB, 8 * MiB) == Decision::KeepWaiting);

    // No further allocation: the rate has collapsed and there is plenty of garbage, so collect.
    EXPECT(policy.evaluate(16 * MiB, 16 * MiB, 8 * MiB) == Decision::Collect);
}

TEST_CASE(rate_drop_is_gated_on_having_enough_garbage)
{
    GC::IdleCollectionPolicy policy;
    policy.reset(0);

    // A burst far below the minimum-garbage gate (threshold / 16, i.e. 0.5 MiB here).
    EXPECT(policy.evaluate(64 * KiB, 64 * KiB, 8 * MiB) == Decision::KeepWaiting);

    // The rate has dropped, but there still isn't enough garbage to be worth marking the whole live heap.
    EXPECT(policy.evaluate(64 * KiB, 64 * KiB, 8 * MiB) == Decision::KeepWaiting);
}

TEST_CASE(watchdog_collects_when_the_rate_never_drops)
{
    GC::IdleCollectionPolicy policy;
    policy.reset(0);

    // Steady allocation every tick never looks like a rate drop, so only the watchdog can fire.
    u64 total = 0;
    for (u32 tick = 1; tick < GC::IdleCollectionPolicy::watchdog_ticks; ++tick) {
        total += MiB;
        EXPECT(policy.evaluate(total, 1 * MiB, 8 * MiB) == Decision::KeepWaiting);
    }

    // The watchdog fires on the final tick regardless of the rate or the gate.
    total += MiB;
    EXPECT(policy.evaluate(total, 1 * MiB, 8 * MiB) == Decision::Collect);
}

TEST_CASE(parks_when_there_is_nothing_to_collect)
{
    GC::IdleCollectionPolicy policy;
    policy.reset(0);
    EXPECT(policy.evaluate(4 * MiB, 0, 8 * MiB) == Decision::Park);
}

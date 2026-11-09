/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/IdleCollectionPolicy.h>
#include <LibGC/Testing.h>
#include <LibTest/TestCase.h>

using Decision = GC::IdleCollectionPolicy::Decision;

namespace {

bool s_abandoned_cell_was_destroyed = false;

class AbandonedCell final : public GC::Cell {
    GC_CELL(AbandonedCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(AbandonedCell);

public:
    virtual ~AbandonedCell() override { s_abandoned_cell_was_destroyed = true; }

private:
    AbandonedCell() = default;

    [[maybe_unused]] FlatPtr m_room_for_a_freelist_entry[2] {};
};

GC_DEFINE_ALLOCATOR(AbandonedCell);

NEVER_INLINE void allocate_abandoned_cell(GC::Heap& heap)
{
    (void)heap.allocate<AbandonedCell>();
}

}

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

TEST_CASE(an_abandoned_object_graph_is_collected_without_further_allocation)
{
    Core::EventLoop event_loop;
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);

    s_abandoned_cell_was_destroyed = false;
    allocate_abandoned_cell(heap);
    GC::scrub_stack_for_testing();

    heap.did_abandon_object_graph();
    event_loop.spin_until([] { return s_abandoned_cell_was_destroyed; });
}

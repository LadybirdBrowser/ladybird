/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibTest/TestCase.h>

namespace {

bool s_cell_that_allocates_when_destroyed_was_destroyed = false;
bool s_reachable_cell_was_destroyed = false;

class SweptCell final : public GC::Cell {
    GC_CELL(SweptCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(SweptCell);

public:
    enum class Role : u8 {
        AllocatesWhenDestroyed,
        Reachable,
        AllocatedByADestructor,
    };

    virtual ~SweptCell() override
    {
        switch (m_role) {
        case Role::AllocatesWhenDestroyed:
            s_cell_that_allocates_when_destroyed_was_destroyed = true;
            (void)heap().allocate<SweptCell>(Role::AllocatedByADestructor);
            break;
        case Role::Reachable:
            s_reachable_cell_was_destroyed = true;
            break;
        case Role::AllocatedByADestructor:
            break;
        }
    }

private:
    explicit SweptCell(Role role)
        : m_role(role)
    {
    }

    Role m_role;
    [[maybe_unused]] FlatPtr m_room_for_a_freelist_entry[2] {};
};

GC_DEFINE_ALLOCATOR(SweptCell);

NEVER_INLINE void scrub_stack()
{
    u8 volatile filler[8 * KiB];
    for (size_t i = 0; i < sizeof(filler); ++i)
        filler[i] = 0;
}

// NB: Both cells come from the same block of a fresh heap, the unreachable one first, so the sweep of that block
//     destroys it before it reaches the reachable one.
NEVER_INLINE GC::Root<SweptCell> allocate_unreachable_then_reachable_cell(GC::Heap& heap)
{
    (void)heap.allocate<SweptCell>(SweptCell::Role::AllocatesWhenDestroyed);
    return GC::make_root(heap.allocate<SweptCell>(SweptCell::Role::Reachable));
}

}

TEST_CASE(allocating_from_a_destructor_during_a_sweep_does_not_free_reachable_cells_of_the_block)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);

    auto reachable_cell = allocate_unreachable_then_reachable_cell(heap);
    scrub_stack();

    // This collection marks the reachable cell and leaves the destruction of the other one to the incremental sweep.
    heap.collect_garbage();
    EXPECT(heap.is_incremental_sweep_active());
    EXPECT(!s_cell_that_allocates_when_destroyed_was_destroyed);

    // Finishing that sweep destroys the unreachable cell, and its allocation starts another collection.
    heap.set_should_collect_on_every_allocation(true);
    heap.collect_garbage();
    heap.set_should_collect_on_every_allocation(false);

    EXPECT(s_cell_that_allocates_when_destroyed_was_destroyed);
    EXPECT(!s_reachable_cell_was_destroyed);
}

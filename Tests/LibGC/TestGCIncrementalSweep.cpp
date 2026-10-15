/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibTest/TestCase.h>

namespace {

bool s_cell_that_allocates_when_destroyed_was_destroyed = false;
bool s_reachable_cell_was_destroyed = false;

class CellOfAnotherType final : public GC::Cell {
    GC_CELL(CellOfAnotherType, GC::Cell);
    GC_DECLARE_ALLOCATOR(CellOfAnotherType);

private:
    CellOfAnotherType() = default;

    [[maybe_unused]] FlatPtr m_room_for_a_freelist_entry[2] {};
};

GC_DEFINE_ALLOCATOR(CellOfAnotherType);

CellOfAnotherType* s_cell_of_another_type_allocated_by_a_destructor = nullptr;

class SweptCell final : public GC::Cell {
    GC_CELL(SweptCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(SweptCell);

public:
    enum class Role : u8 {
        AllocatesWhenDestroyed,
        AllocatesCellOfAnotherTypeWhenDestroyed,
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
        case Role::AllocatesCellOfAnotherTypeWhenDestroyed:
            s_cell_of_another_type_allocated_by_a_destructor = heap().allocate<CellOfAnotherType>().ptr();
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

// NB: Both cells come from the same block of a fresh heap, the unreachable one first, so the sweep of that block
//     destroys it before it reaches the reachable one.
NEVER_INLINE GC::Root<SweptCell> allocate_unreachable_then_reachable_cell(GC::Heap& heap)
{
    (void)heap.allocate<SweptCell>(SweptCell::Role::AllocatesWhenDestroyed);
    return GC::make_root(heap.allocate<SweptCell>(SweptCell::Role::Reachable));
}

NEVER_INLINE GC::Root<CellOfAnotherType> allocate_reachable_cell_of_another_type_then_unreachable_cell(GC::Heap& heap)
{
    auto reachable_cell = GC::make_root(heap.allocate<CellOfAnotherType>());
    (void)heap.allocate<SweptCell>(SweptCell::Role::AllocatesCellOfAnotherTypeWhenDestroyed);
    return reachable_cell;
}

}

TEST_CASE(allocating_from_a_destructor_during_a_sweep_does_not_free_reachable_cells_of_the_block)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);

    auto reachable_cell = allocate_unreachable_then_reachable_cell(heap);
    GC::scrub_stack_for_testing();

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

TEST_CASE(free_cells_that_a_destructor_takes_while_a_collection_finishes_the_sweep_go_back_to_their_block)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);

    auto reachable_cell = allocate_reachable_cell_of_another_type_then_unreachable_cell(heap);
    GC::scrub_stack_for_testing();

    // This collection leaves the destruction of the unreachable cell to the incremental sweep.
    heap.collect_garbage();
    EXPECT(heap.is_incremental_sweep_active());

    // The next collection finishes that sweep. The destructor takes the free cells of the block that holds the
    // reachable cell, and the cell it allocates there is unreachable.
    heap.collect_garbage();
    auto* cell_allocated_by_the_destructor = s_cell_of_another_type_allocated_by_a_destructor;
    EXPECT(cell_allocated_by_the_destructor);
    auto& block = *GC::HeapBlock::from_cell(cell_allocated_by_the_destructor);
    EXPECT_EQ(&block, GC::HeapBlock::from_cell(reachable_cell.ptr()));
    EXPECT(block.is_pending_sweep());

    // Sweeping the block, as the sweep timer would, frees that cell. Once no allocator holds free cells of the block,
    // the next cell comes from the free list of the block.
    heap.sweep_block(block);
    auto next_cell = GC::make_root(heap.allocate<CellOfAnotherType>());
    EXPECT_EQ(next_cell.ptr(), cell_allocated_by_the_destructor);
}

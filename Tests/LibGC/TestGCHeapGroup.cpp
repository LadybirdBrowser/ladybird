/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/CrossHeapMember.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapGroup.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibGC/WeakInlines.h>
#include <LibTest/TestCase.h>

namespace {

size_t s_live_linked_cells = 0;

class LinkedCell final : public GC::Cell {
    GC_CELL(LinkedCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(LinkedCell);

public:
    virtual ~LinkedCell() override { --s_live_linked_cells; }

    GC::CrossHeapMember<LinkedCell>& foreign() { return m_foreign; }
    GC::Ptr<LinkedCell>& local() { return m_local; }

private:
    LinkedCell() { ++s_live_linked_cells; }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_local);
        m_foreign.visit(visitor);
    }

    GC::CrossHeapMember<LinkedCell> m_foreign;
    GC::Ptr<LinkedCell> m_local;
};

GC_DEFINE_ALLOCATOR(LinkedCell);

Optional<bool> s_weak_to_dead_foreign_cell_was_null_while_finalizing;

class WeakCreatingCell final : public GC::Cell {
    GC_CELL(WeakCreatingCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(WeakCreatingCell);

public:
    // Not visited, so that the foreign cell dies whether or not we do.
    GC::RawPtr<LinkedCell> foreign_cell;

private:
    WeakCreatingCell() = default;

    virtual void finalize() override
    {
        Base::finalize();
        s_weak_to_dead_foreign_cell_was_null_while_finalizing = !GC::Weak<LinkedCell> { foreign_cell.ptr() };
    }
};

GC_DEFINE_ALLOCATOR(WeakCreatingCell);

NEVER_INLINE GC::Root<LinkedCell> allocate_holder_and_foreign_target(GC::Heap& holder_heap, GC::Heap& target_heap)
{
    auto holder = GC::make_root(holder_heap.allocate<LinkedCell>());
    auto target = target_heap.allocate<LinkedCell>();
    holder->foreign() = target.ptr();
    return holder;
}

NEVER_INLINE void allocate_cross_heap_cycle(GC::Heap& heap_a, GC::Heap& heap_b)
{
    auto cell_on_a = heap_a.allocate<LinkedCell>();
    auto cell_on_b = heap_b.allocate<LinkedCell>();
    cell_on_a->foreign() = cell_on_b.ptr();
    cell_on_b->foreign() = cell_on_a.ptr();
}

NEVER_INLINE GC::Root<LinkedCell> allocate_cross_heap_chain(GC::Heap& heap_a, GC::Heap& heap_b)
{
    // root -> A -> B -> A(second)
    auto holder = GC::make_root(heap_a.allocate<LinkedCell>());
    auto middle = heap_b.allocate<LinkedCell>();
    auto tail = heap_a.allocate<LinkedCell>();
    middle->foreign() = tail.ptr();
    holder->foreign() = middle.ptr();
    return holder;
}

NEVER_INLINE void allocate_weak_creating_cell_and_foreign_garbage(GC::Heap& heap_a, GC::Heap& heap_b)
{
    auto cell = heap_a.allocate<WeakCreatingCell>();
    cell->foreign_cell = heap_b.allocate<LinkedCell>().ptr();
}

NEVER_INLINE void allocate_garbage(GC::Heap& heap)
{
    (void)heap.allocate<LinkedCell>();
}

}

static NEVER_INLINE void sanity_single_heap_frees_garbage()
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    heap.set_incremental_sweep_enabled(false);

    allocate_garbage(heap);
    EXPECT_EQ(s_live_linked_cells, 1u);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();
    EXPECT_EQ(s_live_linked_cells, 0u);
}

static NEVER_INLINE void incoming_cross_heap_member_roots_local_collection()
{
    GC::Heap heap_a([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    GC::Heap heap_b([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    // No event loop runs during this test; sweep synchronously so frees are observable.
    heap_a.set_incremental_sweep_enabled(false);
    heap_b.set_incremental_sweep_enabled(false);
    GC::HeapGroup group;
    group.add(heap_a);
    group.add(heap_b);

    auto holder = allocate_holder_and_foreign_target(heap_a, heap_b);
    EXPECT_EQ(s_live_linked_cells, 2u);

    // The only thing keeping the target alive is the incoming cross-heap member; B's local collection cannot see the holder on A, so the member registration must root it.
    GC::scrub_stack_for_testing();
    heap_b.collect_garbage();
    EXPECT_EQ(s_live_linked_cells, 2u);

    // Once the edge is dropped, B's next local collection should free the target.
    holder->foreign() = nullptr;
    GC::scrub_stack_for_testing();
    heap_b.collect_garbage();
    EXPECT_EQ(s_live_linked_cells, 1u);

    holder = {};
    GC::scrub_stack_for_testing();
    heap_a.collect_garbage();
    EXPECT_EQ(s_live_linked_cells, 0u);

    group.remove(heap_a);
    group.remove(heap_b);
}

static NEVER_INLINE void group_collection_breaks_cross_heap_cycles()
{
    GC::Heap heap_a([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    GC::Heap heap_b([](auto&) { }, GC::Heap::BecomeProcessDefault::No);

    heap_a.set_incremental_sweep_enabled(false);
    heap_b.set_incremental_sweep_enabled(false);
    GC::HeapGroup group;
    group.add(heap_a);
    group.add(heap_b);

    allocate_cross_heap_cycle(heap_a, heap_b);
    EXPECT_EQ(s_live_linked_cells, 2u);

    // Local collections see the incoming members as roots, so the boundary cycle should survive collection.
    GC::scrub_stack_for_testing();
    heap_a.collect_garbage();
    heap_b.collect_garbage();
    EXPECT_EQ(s_live_linked_cells, 2u);

    // The unified mark should not reach the cycle, so both cells should be freed by the group collection.
    GC::scrub_stack_for_testing();
    group.collect_garbage();
    EXPECT_EQ(s_live_linked_cells, 0u);

    group.remove(heap_a);
    group.remove(heap_b);
}

static NEVER_INLINE void group_collection_traces_live_cross_heap_chains()
{
    GC::Heap heap_a([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    GC::Heap heap_b([](auto&) { }, GC::Heap::BecomeProcessDefault::No);

    heap_a.set_incremental_sweep_enabled(false);
    heap_b.set_incremental_sweep_enabled(false);
    GC::HeapGroup group;
    group.add(heap_a);
    group.add(heap_b);

    auto holder = allocate_cross_heap_chain(heap_a, heap_b);
    EXPECT_EQ(s_live_linked_cells, 3u);

    // The whole chain must survive a group collection, including the second A cell that is only reachable through the B cell.
    GC::scrub_stack_for_testing();
    group.collect_garbage();
    EXPECT_EQ(s_live_linked_cells, 3u);

    holder = {};
    GC::scrub_stack_for_testing();
    group.collect_garbage();
    EXPECT_EQ(s_live_linked_cells, 0u);

    group.remove(heap_a);
    group.remove(heap_b);
}

static NEVER_INLINE void group_collection_finalizer_creates_null_weak_to_dead_foreign_cell()
{
    GC::Heap heap_a([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    GC::Heap heap_b([](auto&) { }, GC::Heap::BecomeProcessDefault::No);

    heap_a.set_incremental_sweep_enabled(false);
    heap_b.set_incremental_sweep_enabled(false);
    GC::HeapGroup group;
    group.add(heap_a);
    group.add(heap_b);

    // The finalizer runs while heap B's weak pointers have already been cleared, whether or not B has run its own
    // finalizers yet, so a weak pointer that it creates to B's dead cell must be null.
    s_weak_to_dead_foreign_cell_was_null_while_finalizing = {};
    allocate_weak_creating_cell_and_foreign_garbage(heap_a, heap_b);
    GC::scrub_stack_for_testing();
    group.collect_garbage();
    EXPECT_EQ(s_weak_to_dead_foreign_cell_was_null_while_finalizing, true);
    EXPECT_EQ(s_live_linked_cells, 0u);

    group.remove(heap_a);
    group.remove(heap_b);
}

// Under ASan with detect_stack_use_after_return, a function's locals live on a fake stack, but its frame on the real
// stack still reserves room for them — room it never writes, so it keeps whatever earlier calls left there, and the
// conservative scan reads it. A stale pointer there can keep a dropped cell alive. So each case runs in a frame of its
// own, on stack that GC::scrub_stack_for_testing() has just cleared.
TEST_CASE(sanity_single_heap_frees_garbage)
{
    GC::scrub_stack_for_testing();
    sanity_single_heap_frees_garbage();
}

TEST_CASE(incoming_cross_heap_member_roots_local_collection)
{
    GC::scrub_stack_for_testing();
    incoming_cross_heap_member_roots_local_collection();
}

TEST_CASE(group_collection_breaks_cross_heap_cycles)
{
    GC::scrub_stack_for_testing();
    group_collection_breaks_cross_heap_cycles();
}

TEST_CASE(group_collection_traces_live_cross_heap_chains)
{
    GC::scrub_stack_for_testing();
    group_collection_traces_live_cross_heap_chains();
}

TEST_CASE(group_collection_finalizer_creates_null_weak_to_dead_foreign_cell)
{
    GC::scrub_stack_for_testing();
    group_collection_finalizer_creates_null_weak_to_dead_foreign_cell();
}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibGC/WeakInlines.h>
#include <LibTest/TestCase.h>

namespace {

class TestCell final : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

public:
    i64 value { 0 };

private:
    TestCell() = default;
};

GC_DEFINE_ALLOCATOR(TestCell);

class CellWithFunction final : public GC::Cell {
    GC_CELL(CellWithFunction, GC::Cell);
    GC_DECLARE_ALLOCATOR(CellWithFunction);

public:
    GC::Ptr<GC::Function<void()>> function;

private:
    CellWithFunction() = default;

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(function);
    }
};

GC_DEFINE_ALLOCATOR(CellWithFunction);

struct DestructionFlag {
    AK_ALLOC_WITH_KMALLOC;

    explicit DestructionFlag(bool& destroyed)
        : destroyed(destroyed)
    {
    }
    ~DestructionFlag() { destroyed = true; }

    bool& destroyed;
};

struct RunState : RefCounted<RunState> {
    GC::Weak<GC::Function<void()>> function;
    bool survived_collection { false };
};

struct Holders {
    GC::Root<GC::Function<int()>> function;
    GC::Weak<TestCell> cell;
};

NEVER_INLINE Holders make_lambda_function_capturing_cell(GC::Heap& heap)
{
    auto cell = heap.allocate<TestCell>();
    cell->value = 7;
    auto function = GC::create_function(heap, [cell] { return static_cast<int>(cell->value); });
    return { GC::make_root(function), cell.ptr() };
}

NEVER_INLINE GC::Weak<GC::Function<void()>> make_unrooted_lambda_function_owning(GC::Heap& heap, bool& destroyed)
{
    auto function = GC::create_function(heap, [flag = make<DestructionFlag>(destroyed)] { });
    return function.ptr();
}

}

TEST_CASE(lambda_captures_are_scanned_conservatively)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    heap.set_incremental_sweep_enabled(false);

    auto holders = make_lambda_function_capturing_cell(heap);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT(holders.cell);
    EXPECT_EQ(holders.function->function()(), 7);
}

TEST_CASE(lambda_captures_are_destroyed_with_the_function)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    heap.set_incremental_sweep_enabled(false);

    bool destroyed = false;
    auto function = make_unrooted_lambda_function_owning(heap, destroyed);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT(!function);
    EXPECT(destroyed);
}

TEST_CASE(function_stays_alive_while_it_runs)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    heap.set_incremental_sweep_enabled(false);

    auto state = adopt_ref(*new RunState);
    auto holder = GC::make_root(heap.allocate<CellWithFunction>());
    holder->function = GC::create_function(heap, [holder = GC::Ref { *holder }, state] {
        holder->function = nullptr;
        holder->heap().collect_garbage();
        state->survived_collection = static_cast<bool>(state->function);
    });
    state->function = holder->function;

    holder->function->function()();
    EXPECT(state->survived_collection);
}

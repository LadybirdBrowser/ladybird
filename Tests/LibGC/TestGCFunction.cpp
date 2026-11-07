/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/CompressedPointer.h>
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

class CellCallingFunctionWhenFinalized final : public GC::Cell {
    GC_CELL(CellCallingFunctionWhenFinalized, GC::Cell);
    GC_DECLARE_ALLOCATOR(CellCallingFunctionWhenFinalized);

public:
    GC::Ptr<GC::Function<void()>> function;

private:
    CellCallingFunctionWhenFinalized() = default;

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(function);
    }

    virtual void finalize() override
    {
        Base::finalize();
        function->function()();
    }
};

GC_DEFINE_ALLOCATOR(CellCallingFunctionWhenFinalized);

struct DestructionFlag {
    GC_HOLDS_NO_CELL_EDGES;

    AK_ALLOC_WITH_KMALLOC;

    explicit DestructionFlag(bool& destroyed)
        : destroyed(destroyed)
    {
    }
    ~DestructionFlag() { destroyed = true; }

    bool& destroyed;
};

struct RunState : RefCounted<RunState> {
    GC_HOLDS_NO_CELL_EDGES;

    GC::Weak<GC::Function<void()>> function;
    bool survived_collection { false };
};

struct CalledFlag : RefCounted<CalledFlag> {
    GC_HOLDS_NO_CELL_EDGES;

    explicit CalledFlag(bool& called)
        : called(called)
    {
    }

    bool& called;
};

struct CountedWithCell : RefCounted<CountedWithCell> {
    GC::Ptr<TestCell> cell;
};

struct CountedWithoutCells : RefCounted<CountedWithoutCells> {
    GC_HOLDS_NO_CELL_EDGES;
};

struct CountedAndTraced : RefCounted<CountedAndTraced> {
    GC::Ptr<TestCell> cell;

    void visit_edges(GC::Cell::Visitor& visitor) { visitor.visit(cell); }
};

// Nothing can reach a capture without saying how it is traced, also not through a counted or owning pointer.
static_assert(!GC::IsCapturable<CountedWithCell>);
static_assert(!GC::IsCapturable<NonnullRefPtr<CountedWithCell>>);
static_assert(!GC::IsCapturable<RefPtr<CountedWithCell>>);
static_assert(!GC::IsCapturable<OwnPtr<CountedWithCell>>);
static_assert(!GC::IsCapturable<NonnullOwnPtr<CountedWithCell>>);
static_assert(!GC::IsCapturable<Vector<NonnullRefPtr<CountedWithCell>>>);
static_assert(!GC::IsCapturable<AK::Function<void()>>);
static_assert(!GC::IsCapturable<int*>);
static_assert(GC::IsCapturable<NonnullRefPtr<CountedWithoutCells>>);
static_assert(GC::IsCapturable<NonnullRefPtr<CountedWithoutCells const>>);
static_assert(GC::IsCapturable<OwnPtr<CountedWithoutCells>>);
static_assert(GC::IsCapturable<NonnullRefPtr<CountedAndTraced>>);
static_assert(GC::IsCapturable<Optional<RefPtr<CountedAndTraced>>>);

// A root is only capturable on its own, which holds it as a pointer, so that a function never keeps a root.
static_assert(!GC::IsCapturable<GC::Root<TestCell>>);
static_assert(!GC::IsCapturable<Optional<GC::Root<TestCell>>>);
static_assert(!GC::IsCapturable<Vector<GC::Root<TestCell>>>);
static_assert(IsSame<GC::Detail::CaptureStorage<GC::Root<TestCell>&>::Type, GC::Detail::CapturedRoot<TestCell>>);

struct Holders {
    GC::Root<GC::Function<int()>> function;
    GC::Weak<TestCell> cell;
};

NEVER_INLINE Holders make_function_capturing_cell(GC::Heap& heap)
{
    auto cell = heap.allocate<TestCell>();
    cell->value = 42;
    auto function = GC::create_function(heap, GC::captures(*cell), [](TestCell& cell) { return static_cast<int>(cell.value); });
    return { GC::make_root(function), cell.ptr() };
}

NEVER_INLINE Holders make_function_capturing_cell_address_as_integers(GC::Heap& heap)
{
    auto cell = heap.allocate<TestCell>();
    auto address = bit_cast<FlatPtr>(cell.ptr());
    auto compressed = GC::compress(cell.ptr());
    auto function = GC::create_function(heap, GC::captures(address, compressed), [](FlatPtr address, u32 compressed) {
        return static_cast<int>(address != 0) + static_cast<int>(compressed != 0);
    });
    return { GC::make_root(function), cell.ptr() };
}

NEVER_INLINE GC::Weak<CellWithFunction> make_cycle_through_captured_root(GC::Heap& heap)
{
    auto cell = heap.allocate<CellWithFunction>();
    auto root = GC::make_root(cell);
    cell->function = GC::create_function(heap, GC::captures(root), [](GC::Ptr<CellWithFunction>&) { });
    return cell.ptr();
}

NEVER_INLINE GC::Weak<GC::Function<void()>> make_unrooted_function_owning(GC::Heap& heap, bool& destroyed)
{
    auto function = GC::create_function(heap, GC::captures(make<DestructionFlag>(destroyed)), [](NonnullOwnPtr<DestructionFlag>&) { });
    return function.ptr();
}

NEVER_INLINE void make_unrooted_cell_calling_function_when_finalized(GC::Heap& heap, bool& called)
{
    // NB: Finalizers run in no particular order. The function is made first, which here finalizes it first.
    auto function = GC::create_function(heap, GC::captures(make_ref_counted<CalledFlag>(called)), [](NonnullRefPtr<CalledFlag>& flag) {
        flag->called = true;
    });
    heap.allocate<CellCallingFunctionWhenFinalized>()->function = function;
}

}

TEST_CASE(captured_cell_is_traced)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    heap.set_incremental_sweep_enabled(false);

    auto holders = make_function_capturing_cell(heap);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT(holders.cell);
    EXPECT_EQ(holders.function->function()(), 42);
}

TEST_CASE(captured_integers_are_not_cell_pointers)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    heap.set_incremental_sweep_enabled(false);

    auto holders = make_function_capturing_cell_address_as_integers(heap);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT(!holders.cell);
    EXPECT_EQ(holders.function->function()(), 2);
}

TEST_CASE(captures_take_arguments_after_them_and_keep_state)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);

    auto function = GC::create_function(heap, GC::captures(0), [](int& calls, int argument) {
        ++calls;
        return calls * 100 + argument;
    });

    EXPECT_EQ(function->function()(1), 101);
    EXPECT_EQ(function->function()(2), 202);
}

TEST_CASE(function_without_captures)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);

    auto function = GC::create_function(heap, [](int argument) { return argument * 2; });
    EXPECT_EQ(function->function()(21), 42);
}

TEST_CASE(captures_are_destroyed_with_the_function)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    heap.set_incremental_sweep_enabled(false);

    bool destroyed = false;
    auto function = make_unrooted_function_owning(heap, destroyed);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT(!function);
    EXPECT(destroyed);
}

TEST_CASE(dead_function_can_be_called_by_the_finalizer_of_another_dead_cell)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    heap.set_incremental_sweep_enabled(false);

    bool called = false;
    make_unrooted_cell_calling_function_when_finalized(heap, called);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT(called);
}

TEST_CASE(captures_taken_as_auto_with_a_named_function_type)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);

    auto function = GC::create_function<int(int)>(heap, GC::captures(1, 2), [](auto& first, auto& second, int argument) {
        return first + second + argument;
    });
    EXPECT_EQ(function->function()(3), 6);
}

TEST_CASE(captures_taken_as_auto_with_a_deduced_function_type)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    auto cell = GC::make_root(heap.allocate<TestCell>());
    cell->value = 5;

    auto function = GC::create_function(heap, GC::captures(*cell, 10), [](auto& cell, auto& offset, int argument) {
        return static_cast<int>(cell.value) + offset + argument;
    });
    static_assert(IsSame<decltype(function), GC::Ref<GC::Function<int(int)>>>);
    EXPECT_EQ(function->function()(1), 16);
}

TEST_CASE(captured_root_does_not_keep_a_cycle_alive)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    heap.set_incremental_sweep_enabled(false);

    auto cell = make_cycle_through_captured_root(heap);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT(!cell);
}

TEST_CASE(captured_root_vector_is_traced_as_a_vector)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::Yes);
    heap.set_incremental_sweep_enabled(false);

    GC::RootVector<GC::Ref<TestCell>> cells;
    cells.append(heap.allocate<TestCell>());
    cells[0]->value = 3;
    auto function = GC::make_root(GC::create_function(heap, GC::captures(move(cells)), [](Vector<GC::Ref<TestCell>>& cells) {
        return static_cast<int>(cells[0]->value);
    }));
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT_EQ(function->function()(), 3);
}

TEST_CASE(function_with_captures_stays_alive_while_it_runs)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::No);
    heap.set_incremental_sweep_enabled(false);

    auto state = adopt_ref(*new RunState);
    auto holder = GC::make_root(heap.allocate<CellWithFunction>());
    holder->function = GC::create_function(heap, GC::captures(*holder, state), [](CellWithFunction& holder, NonnullRefPtr<RunState>& state) {
        holder.function = nullptr;
        holder.heap().collect_garbage();
        state->survived_collection = static_cast<bool>(state->function);
    });
    state->function = holder->function;

    holder->function->function()();
    EXPECT(state->survived_collection);
}

TEST_CASE(captures_too_large_for_their_entry_keep_their_address_as_the_table_grows)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeThreadDefault::Yes);

    Array<u64, 8> values { 1, 2, 3, 4, 5, 6, 7, 8 };
    auto function = GC::make_root(GC::create_function(heap, GC::captures(values, 0), [](Array<u64, 8>& values, int& padding) {
        auto const* before = values.data();
        for (size_t i = 0; i < 1000; ++i)
            (void)GC::create_function(GC::Heap::the(), [] { });
        VERIFY(values.data() == before);
        u64 sum = 0;
        for (auto value : values)
            sum += value;
        return static_cast<int>(sum) + padding;
    }));
    EXPECT_EQ(function->function()(), 36);
}

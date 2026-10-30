/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NeverDestroyed.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/Member.h>
#include <LibGC/Root.h>
#include <LibTest/TestCase.h>

namespace {

class TestCell : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

public:
    GC::Member<TestCell> next;
    u32 value { 0 };

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(next);
    }
};

GC_DEFINE_ALLOCATOR(TestCell);

class DerivedTestCell final : public TestCell {
    GC_CELL(DerivedTestCell, TestCell);
    GC_DECLARE_ALLOCATOR(DerivedTestCell);
};

GC_DEFINE_ALLOCATOR(DerivedTestCell);

static GC::Heap& test_heap()
{
    static AK::NeverDestroyed<GC::Heap> heap([](auto&) { });
    return *heap;
}

TEST_SETUP
{
    GC::Heap::set_default_heap_for_testing(test_heap());
}

static_assert(sizeof(GC::Member<TestCell>) == sizeof(u32));
static_assert(sizeof(GC::NonnullMember<TestCell>) == sizeof(u32));
static_assert(IsTriviallyCopyConstructible<GC::Member<TestCell>> && IsTriviallyMoveConstructible<GC::Member<TestCell>>);
static_assert(IsTriviallyDestructible<GC::Member<TestCell>>);
static_assert(IsTriviallyCopyConstructible<GC::NonnullMember<TestCell>> && IsTriviallyMoveConstructible<GC::NonnullMember<TestCell>>);
static_assert(IsTriviallyDestructible<GC::NonnullMember<TestCell>>);

TEST_CASE(base_copy_matches_the_heap_region)
{
    // NB: The first cell reserves the region, if nothing has yet.
    (void)test_heap().allocate<TestCell>();
    EXPECT_EQ(GC::heap_region_base(), js_heap_region_base);
    EXPECT_EQ(GC::heap_region_base() % GC::HEAP_REGION_SIZE, 0u);
}

TEST_CASE(base_copy_registered_after_the_region_exists_gets_the_base)
{
    (void)test_heap().allocate<TestCell>();
    // NB: Registrations are never undone, so this one is for the rest of the process.
    static FlatPtr copy;
    static GC::HeapRegionBaseCopyRegistration registration { copy };
    EXPECT_NE(copy, 0u);
    EXPECT_EQ(copy, js_heap_region_base);
}

TEST_CASE(member_round_trips_and_null_is_zero)
{
    auto cell = test_heap().allocate<TestCell>();

    GC::Member<TestCell> member;
    EXPECT(!member);
    EXPECT_EQ(member.compressed(), 0u);
    EXPECT_EQ(member.ptr(), nullptr);
    EXPECT(member == nullptr);

    member = cell;
    EXPECT(member);
    EXPECT_EQ(member.ptr(), cell.ptr());
    EXPECT_EQ(member.compressed(), GC::compress(cell.ptr()));
    EXPECT_EQ(&*member, cell.ptr());
    EXPECT_EQ(member->value, 0u);

    member = nullptr;
    EXPECT(!member);

    GC::NonnullMember<TestCell> nonnull = *cell;
    EXPECT_EQ(nonnull.ptr(), cell.ptr());
    EXPECT_EQ(nonnull.compressed(), GC::compress(cell.ptr()));
}

TEST_CASE(members_convert_to_and_from_stack_pointers)
{
    auto derived = test_heap().allocate<DerivedTestCell>();

    GC::Member<DerivedTestCell> member = derived;
    GC::Ptr<TestCell> base_ptr = member;
    EXPECT_EQ(base_ptr.ptr(), derived.ptr());
    GC::Ptr<GC::Cell> cell_ptr = member;
    EXPECT_EQ(cell_ptr.ptr(), derived.ptr());
    EXPECT_EQ(member.as_nonnull().ptr(), derived.ptr());

    GC::NonnullMember<DerivedTestCell> nonnull = derived;
    GC::Ref<TestCell> base_ref = nonnull;
    EXPECT_EQ(base_ref.ptr(), derived.ptr());
    GC::Ptr<TestCell> base_ptr_from_nonnull = nonnull;
    EXPECT_EQ(base_ptr_from_nonnull.ptr(), derived.ptr());
    TestCell& reference = nonnull;
    EXPECT_EQ(&reference, derived.ptr());

    GC::Member<TestCell> widened = nonnull;
    EXPECT_EQ(widened.ptr(), derived.ptr());
    GC::Member<TestCell> from_ptr = GC::Ptr<DerivedTestCell> { derived };
    EXPECT_EQ(from_ptr.ptr(), derived.ptr());
    GC::NonnullMember<TestCell> from_ref = derived;
    EXPECT_EQ(from_ref.ptr(), derived.ptr());
}

TEST_CASE(members_compare_like_pointers)
{
    auto a = test_heap().allocate<TestCell>();
    auto b = test_heap().allocate<DerivedTestCell>();

    GC::Member<TestCell> member_a = a;
    GC::Member<TestCell> member_b = b;
    GC::NonnullMember<DerivedTestCell> nonnull_b = b;

    EXPECT(member_a != member_b);
    EXPECT(member_b == nonnull_b);
    EXPECT(nonnull_b == member_b);
    EXPECT(member_a == a);
    EXPECT(a == member_a);
    EXPECT(member_a != GC::Ptr<TestCell> { b });
    EXPECT(member_b == GC::Ref<DerivedTestCell> { *b });
    EXPECT(member_a == a.ptr());
    EXPECT(b.ptr() == nonnull_b);
    EXPECT(member_a != nullptr);
    EXPECT(GC::Member<TestCell> {} == GC::Ptr<TestCell> {});

    GC::Cell* as_cell = b.ptr();
    EXPECT(member_b == as_cell);
    EXPECT(nonnull_b == as_cell);
}

TEST_CASE(members_hash_like_pointers)
{
    auto a = test_heap().allocate<TestCell>();
    auto b = test_heap().allocate<TestCell>();

    EXPECT_EQ(Traits<GC::Member<TestCell>>::hash(a), Traits<GC::Ptr<TestCell>>::hash(a));
    EXPECT_EQ(Traits<GC::NonnullMember<TestCell>>::hash(*a), Traits<GC::Ref<TestCell>>::hash(a));

    HashTable<GC::Member<TestCell>> table;
    table.set(a);
    EXPECT(table.contains(GC::Ptr<TestCell> { a }));
    EXPECT(!table.contains(GC::Ptr<TestCell> { b }));

    HashMap<GC::NonnullMember<TestCell>, int> map;
    map.set(*a, 1);
    map.set(*b, 2);
    EXPECT_EQ(map.get(GC::Ref<TestCell> { a }), 1);
    EXPECT_EQ(map.get(GC::Ref<TestCell> { b }), 2);
}

class TestVisitor : public GC::Cell::Visitor {
    virtual void visit_impl(GC::Cell& cell) override { visited_cells.append(&cell); }
    virtual void visit_impl(ReadonlySpan<GC::NanBoxedValue>) override { }
    virtual void visit_possible_values(ReadonlyBytes) override { }

public:
    Vector<GC::Cell*> visited_cells;
};

TEST_CASE(visitor_visits_member_slots)
{
    auto a = test_heap().allocate<TestCell>();
    auto b = test_heap().allocate<TestCell>();

    GC::Member<TestCell> member = a;
    GC::Member<TestCell> null_member;
    GC::NonnullMember<TestCell> nonnull = *b;
    Vector<GC::Member<TestCell>> vector { a, nullptr, b };
    Optional<GC::NonnullMember<TestCell>> optional = GC::NonnullMember<TestCell> { *a };

    TestVisitor visitor;
    visitor.visit(member);
    visitor.visit(null_member);
    visitor.visit(nonnull);
    visitor.visit(vector);
    visitor.visit(optional);

    Vector<GC::Cell*> expected { a.ptr(), b.ptr(), a.ptr(), b.ptr(), a.ptr() };
    EXPECT_EQ(visitor.visited_cells, expected);
}

TEST_CASE(members_keep_cells_alive)
{
    auto head = GC::make_root(test_heap().allocate<TestCell>());
    head->next = test_heap().allocate<TestCell>();
    head->next->value = 42;

    test_heap().collect_garbage(GC::Heap::CollectionType::CollectGarbage);

    EXPECT_EQ(head->next->value, 42u);
}

class FinalizationTrackingCell final : public GC::Cell {
    GC_CELL(FinalizationTrackingCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(FinalizationTrackingCell);

public:
    explicit FinalizationTrackingCell(bool& finalized)
        : m_finalized(finalized)
    {
    }

private:
    virtual void finalize() override
    {
        Base::finalize();
        m_finalized = true;
    }

    bool& m_finalized;
};

GC_DEFINE_ALLOCATOR(FinalizationTrackingCell);

// Keeps nothing on the stack that points at the cell the function captures, other than the root of the function.
template<typename MakeCallable>
static NEVER_INLINE GC::Root<GC::Function<size_t()>> make_function_capturing_a_member(bool& finalized, MakeCallable make_callable)
{
    GC::Member<FinalizationTrackingCell> member = test_heap().allocate<FinalizationTrackingCell>(finalized);
    return GC::make_root(GC::create_function(test_heap(), make_callable(member)));
}

static NEVER_INLINE void clobber_the_stack()
{
    u8 volatile scratch[16 * KiB];
    for (auto& byte : scratch)
        byte = 0;
}

TEST_CASE(function_whose_only_capture_is_a_member_keeps_its_cell_alive)
{
    // NB: Static, since the cell outlives the test once nothing holds it anymore.
    static bool finalized;
    finalized = false;
    auto function = make_function_capturing_a_member(finalized, [](GC::Member<FinalizationTrackingCell> member) {
        auto callable = [member] { return reinterpret_cast<size_t>(member.ptr()); };
        static_assert(sizeof(callable) == sizeof(u32));
        return callable;
    });
    clobber_the_stack();

    test_heap().collect_garbage(GC::Heap::CollectionType::CollectGarbage);

    EXPECT(!finalized);
    EXPECT_NE(function->function()(), 0u);
}

TEST_CASE(function_whose_captures_end_in_a_member_keeps_its_cell_alive)
{
    // NB: Static, since the cell outlives the test once nothing holds it anymore.
    static bool finalized;
    finalized = false;
    auto function = make_function_capturing_a_member(finalized, [](GC::Member<FinalizationTrackingCell> member) {
        auto callable = [first = static_cast<u32>(1), second = static_cast<u32>(2), member] {
            return reinterpret_cast<size_t>(member.ptr()) + first + second;
        };
        // NB: The member is past the last whole word of the captures.
        static_assert(sizeof(callable) == 3 * sizeof(u32));
        return callable;
    });
    clobber_the_stack();

    test_heap().collect_garbage(GC::Heap::CollectionType::CollectGarbage);

    EXPECT(!finalized);
    EXPECT_NE(function->function()(), 0u);
}

// Walks a list of cells through compressed pointers, decoding them against the base copy or against the base exported
// from LibGC. The opaque call per cell keeps the compiler from hoisting the base out of the loop, as an ordinary DOM
// walk with calls in its body would.
static NEVER_INLINE void touch(TestCell& cell)
{
    cell.value += 1;
    asm volatile("" ::: "memory");
}

static GC::Root<TestCell> const& benchmark_list()
{
    static GC::Root<TestCell> head = [] {
        auto head = GC::make_root(test_heap().allocate<TestCell>());
        GC::Ptr<TestCell> tail = head.ptr();
        for (size_t i = 0; i < 100'000; ++i) {
            auto cell = test_heap().allocate<TestCell>();
            tail->next = cell;
            tail = cell;
        }
        return head;
    }();
    return head;
}

template<typename LoadBase>
static NEVER_INLINE void walk_benchmark_list(LoadBase load_base)
{
    auto* head = benchmark_list().ptr();
    for (size_t round = 0; round < 1000; ++round) {
        for (auto* cell = head; cell;) {
            touch(*cell);
            auto compressed = cell->next.compressed();
            cell = compressed ? reinterpret_cast<TestCell*>(GC::decompress_nonnull(compressed, load_base())) : nullptr;
        }
    }
}

// The same walk, with the decode in a function of its own, so it loads the base (and its address) on every call.
template<typename LoadBase>
static NEVER_INLINE TestCell* next_cell(TestCell& cell, LoadBase load_base)
{
    auto compressed = cell.next.compressed();
    return compressed ? reinterpret_cast<TestCell*>(GC::decompress_nonnull(compressed, load_base())) : nullptr;
}

template<typename LoadBase>
static NEVER_INLINE void walk_benchmark_list_out_of_line(LoadBase load_base)
{
    auto* head = benchmark_list().ptr();
    for (size_t round = 0; round < 1000; ++round) {
        for (auto* cell = head; cell; cell = next_cell(*cell, load_base))
            touch(*cell);
    }
}

BENCHMARK_CASE(walk_members_out_of_line_with_exported_base)
{
    walk_benchmark_list_out_of_line([] { return js_heap_region_base; });
}

BENCHMARK_CASE(walk_members_out_of_line_with_base_copy)
{
    walk_benchmark_list_out_of_line([] { return GC::heap_region_base(); });
}

BENCHMARK_CASE(walk_members_with_base_copy)
{
    walk_benchmark_list([] { return GC::heap_region_base(); });
}

BENCHMARK_CASE(walk_members_with_exported_base)
{
    walk_benchmark_list([] { return js_heap_region_base; });
}

}

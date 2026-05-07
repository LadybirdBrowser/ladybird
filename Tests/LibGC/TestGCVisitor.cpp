/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Ptr.h>
#include <LibTest/TestCase.h>

class TestCell : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

    u8 m_padding[16] {};
};

GC_DEFINE_ALLOCATOR(TestCell);

static GC::Heap& test_heap()
{
    static GC::Heap heap([](auto&) { });
    return heap;
}

class TestVisitor : public GC::Cell::Visitor {
    virtual void visit_impl(GC::Cell& cell) override { visited_cells.set(&cell); }
    virtual void visit_impl(ReadonlySpan<GC::NanBoxedValue> span) override { last_nan_span_size = span.size(); }
    virtual void visit_possible_values(ReadonlyBytes) override { }

public:
    HashTable<GC::Cell*> visited_cells;
    Optional<size_t> last_nan_span_size;
};

class TestNanBox : public GC::NanBoxedValue {
public:
    static TestNanBox from_cell(GC::Cell* cell)
    {
        TestNanBox box;
        box.m_value.encoded = GC::SHIFTED_IS_CELL_PATTERN | (bit_cast<u64>(cell) & 0x0000FFFFFFFFFFFFULL);
        return box;
    }

    static TestNanBox from_double(double value)
    {
        TestNanBox box;
        box.m_value.as_double = value;
        return box;
    }
};

TEST_CASE(visit_cell_pointer_traces_cell)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    TestVisitor visitor;
    visitor.visit(static_cast<GC::Cell*>(cell.ptr()));

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_null_cell_pointer_traces_nothing)
{
    TestVisitor visitor;
    visitor.visit(static_cast<GC::Cell*>(nullptr));

    EXPECT_EQ(visitor.visited_cells.size(), 0u);
}

TEST_CASE(visit_cell_reference_traces_cell)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    TestVisitor visitor;
    visitor.visit(static_cast<GC::Cell&>(*cell));

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_const_cell_pointer_traces_cell)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    TestVisitor visitor;
    visitor.visit(static_cast<GC::Cell const*>(cell.ptr()));

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_const_cell_reference_traces_cell)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    TestVisitor visitor;
    visitor.visit(static_cast<GC::Cell const&>(*cell));

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_ptr_traces_cell)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();
    GC::Ptr<TestCell> ptr { cell };

    TestVisitor visitor;
    visitor.visit(ptr);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_null_ptr_traces_nothing)
{
    GC::Ptr<TestCell> ptr;

    TestVisitor visitor;
    visitor.visit(ptr);

    EXPECT_EQ(visitor.visited_cells.size(), 0u);
}

TEST_CASE(visit_ref_traces_cell)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();
    GC::Ref<TestCell> ref { cell };

    TestVisitor visitor;
    visitor.visit(ref);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_span_traces_cell_elements)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    Vector<GC::Ref<TestCell>> elements;
    elements.append(cell);

    TestVisitor visitor;
    visitor.visit(elements.span());

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_readonly_span_traces_cell_elements)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    Vector<GC::Ref<TestCell>> elements;
    elements.append(cell);

    TestVisitor visitor;
    visitor.visit(static_cast<ReadonlySpan<GC::Ref<TestCell>>>(elements.span()));

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_vector_traces_cell_elements)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    Vector<GC::Ref<TestCell>> vector;
    vector.append(cell);

    TestVisitor visitor;
    visitor.visit(vector);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_hash_table_traces_cell_elements)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    HashTable<GC::Ref<TestCell>> table;
    table.set(cell);

    TestVisitor visitor;
    visitor.visit(table);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_ordered_hash_table_traces_cell_elements)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    OrderedHashTable<GC::Ref<TestCell>> table;
    table.set(cell);

    TestVisitor visitor;
    visitor.visit(table);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_hash_map_traces_cell_value)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    HashMap<int, GC::Ref<TestCell>> map;
    map.set(7, cell);

    TestVisitor visitor;
    visitor.visit(map);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_hash_map_traces_cell_key)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    HashMap<GC::Ref<TestCell>, int> map;
    map.set(cell, 7);

    TestVisitor visitor;
    visitor.visit(map);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_ordered_hash_map_traces_cell_value)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();

    OrderedHashMap<int, GC::Ref<TestCell>> map;
    map.set(7, cell);

    TestVisitor visitor;
    visitor.visit(map);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_optional_traces_cell_value)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();
    Optional<GC::Ref<TestCell>> optional { cell };

    TestVisitor visitor;
    visitor.visit(optional);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_empty_optional_traces_nothing)
{
    Optional<GC::Ref<TestCell>> optional;

    TestVisitor visitor;
    visitor.visit(optional);

    EXPECT_EQ(visitor.visited_cells.size(), 0u);
}

TEST_CASE(visit_nan_boxed_value_traces_cell_when_cell_held)
{
    auto& heap = test_heap();
    auto cell = heap.allocate<TestCell>();
    auto value = TestNanBox::from_cell(cell.ptr());

    TestVisitor visitor;
    visitor.visit(value);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(visit_nan_boxed_value_traces_nothing_when_not_cell)
{
    auto value = TestNanBox::from_double(3.14);

    TestVisitor visitor;
    visitor.visit(value);

    EXPECT_EQ(visitor.visited_cells.size(), 0u);
}

TEST_CASE(visit_readonly_span_of_nan_boxed_values_routes_to_visit_impl)
{
    Vector<TestNanBox> values;
    values.append(TestNanBox::from_double(1.0));
    values.append(TestNanBox::from_double(2.0));
    values.append(TestNanBox::from_double(3.0));

    TestVisitor visitor;
    visitor.visit(static_cast<ReadonlySpan<TestNanBox>>(values.span()));

    EXPECT_EQ(visitor.last_nan_span_size, 3u);
}

TEST_CASE(visit_span_of_nan_boxed_values_routes_to_visit_impl)
{
    Vector<TestNanBox> values;
    values.append(TestNanBox::from_double(1.0));
    values.append(TestNanBox::from_double(2.0));

    TestVisitor visitor;
    visitor.visit(values.span());

    EXPECT_EQ(visitor.last_nan_span_size, 2u);
}

TEST_CASE(visit_vector_of_nan_boxed_values_routes_to_visit_impl)
{
    Vector<TestNanBox> values;
    values.append(TestNanBox::from_double(1.0));
    values.append(TestNanBox::from_double(2.0));
    values.append(TestNanBox::from_double(3.0));
    values.append(TestNanBox::from_double(4.0));

    TestVisitor visitor;
    visitor.visit(values);

    EXPECT_EQ(visitor.last_nan_span_size, 4u);
}

static_assert(GC::IsVisitable<Vector<GC::Ref<TestCell>>>::value);
static_assert(GC::IsVisitable<Span<GC::Ref<TestCell>>>::value);
static_assert(GC::IsVisitable<ReadonlySpan<GC::Ref<TestCell>>>::value);
static_assert(GC::IsVisitable<HashTable<GC::Ref<TestCell>>>::value);
static_assert(GC::IsVisitable<OrderedHashTable<GC::Ref<TestCell>>>::value);
static_assert(GC::IsVisitable<Optional<GC::Ref<TestCell>>>::value);

static_assert(GC::IsVisitable<Vector<Vector<GC::Ref<TestCell>>>>::value);

static_assert(!GC::IsVisitable<Vector<int>>::value);
static_assert(!GC::IsVisitable<Span<int>>::value);
static_assert(!GC::IsVisitable<ReadonlySpan<int>>::value);
static_assert(!GC::IsVisitable<HashTable<int>>::value);
static_assert(!GC::IsVisitable<OrderedHashTable<int>>::value);
static_assert(!GC::IsVisitable<Optional<int>>::value);

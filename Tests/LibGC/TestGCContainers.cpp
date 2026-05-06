/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapHashTable.h>
#include <LibGC/HeapVector.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootVector.h>
#include <LibTest/TestCase.h>

class TestCell : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

    // Padding to satisfy minimum cell size (must be >= sizeof(FreelistEntry)).
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
    virtual void visit_impl(ReadonlySpan<GC::NanBoxedValue>) override { }
    virtual void visit_possible_values(ReadonlyBytes) override { }

public:
    HashTable<GC::Cell*> visited_cells;
};

static bool possible_values_contain(GC::ConservativeVectorBase const& container, GC::Cell* cell)
{
    auto target = bit_cast<FlatPtr>(cell);
    for (auto value : container.possible_values()) {
        if (value == target)
            return true;
    }
    return false;
}

TEST_CASE(root_vector_reports_roots)
{
    auto& heap = test_heap();
    GC::RootVector<GC::Ref<TestCell>> vector(heap);

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(root_vector_ptr_reports_roots)
{
    auto& heap = test_heap();
    GC::RootVector<GC::Ptr<TestCell>> vector(heap);

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(root_hash_map_value_reports_roots)
{
    auto& heap = test_heap();
    GC::RootHashMap<int, GC::Ref<TestCell>> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    map.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(root_hash_map_key_reports_roots)
{
    auto& heap = test_heap();
    GC::RootHashMap<GC::Ref<TestCell>, int> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(cell, 42);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    map.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(root_hash_map_key_and_value_reports_roots)
{
    auto& heap = test_heap();
    GC::RootHashMap<GC::Ref<TestCell>, GC::Ref<TestCell>> map(heap);

    auto key_cell = heap.allocate<TestCell>();
    auto value_cell = heap.allocate<TestCell>();
    map.set(key_cell, value_cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    map.gather_roots(roots);

    EXPECT(roots.contains(key_cell.ptr()));
    EXPECT(roots.contains(value_cell.ptr()));
    EXPECT_EQ(roots.size(), 2u);
}

TEST_CASE(root_hash_map_non_gc_key_skipped)
{
    auto& heap = test_heap();
    GC::RootHashMap<int, GC::Ref<TestCell>> map(heap);

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    map.gather_roots(roots);

    // Only the value should be reported, not the int key
    EXPECT_EQ(roots.size(), 1u);
    EXPECT(roots.contains(cell.ptr()));
}

TEST_CASE(cleared_container_reports_no_roots)
{
    auto& heap = test_heap();
    GC::RootVector<GC::Ref<TestCell>> vector(heap);

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);
    vector.clear();

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);

    EXPECT_EQ(roots.size(), 0u);
}

TEST_CASE(conservative_vector_reports_possible_values)
{
    auto& heap = test_heap();
    GC::ConservativeVector<GC::Ref<TestCell>> vector(heap);

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);

    EXPECT(possible_values_contain(vector, cell.ptr()));
}

TEST_CASE(heap_vector_visit_edges_reports_cells)
{
    auto& heap = test_heap();
    auto vector = heap.allocate<GC::HeapVector<GC::Ref<TestCell>>>();

    auto cell = heap.allocate<TestCell>();
    vector->elements().append(cell);

    TestVisitor visitor;
    vector->visit_edges(visitor);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(empty_heap_vector_visit_edges_reports_nothing)
{
    auto& heap = test_heap();
    auto vector = heap.allocate<GC::HeapVector<GC::Ref<TestCell>>>();

    TestVisitor visitor;
    vector->visit_edges(visitor);

    EXPECT_EQ(visitor.visited_cells.size(), 0u);
}

TEST_CASE(heap_hash_table_visit_edges_reports_cells)
{
    auto& heap = test_heap();
    auto table = heap.allocate<GC::HeapHashTable<GC::Ref<TestCell>>>();

    auto cell = heap.allocate<TestCell>();
    table->table().set(cell);

    TestVisitor visitor;
    table->visit_edges(visitor);

    EXPECT(visitor.visited_cells.contains(cell.ptr()));
}

TEST_CASE(empty_heap_hash_table_visit_edges_reports_nothing)
{
    auto& heap = test_heap();
    auto table = heap.allocate<GC::HeapHashTable<GC::Ref<TestCell>>>();

    TestVisitor visitor;
    table->visit_edges(visitor);

    EXPECT_EQ(visitor.visited_cells.size(), 0u);
}

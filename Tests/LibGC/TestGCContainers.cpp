/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/ActivityRoot.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/ConservativeHashMap.h>
#include <LibGC/ConservativeHashTable.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapHashTable.h>
#include <LibGC/HeapVector.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakHashMap.h>
#include <LibTest/TestCase.h>

class TestCell : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

    // Padding to satisfy minimum cell size (must be >= sizeof(FreelistEntry)).
    u8 m_padding[16] {};
};

GC_DEFINE_ALLOCATOR(TestCell);

class FinalizerObservingCell : public GC::Cell {
    GC_CELL(FinalizerObservingCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(FinalizerObservingCell);

public:
    GC::Weak<TestCell> weak_cell;
    bool* weak_cell_was_null_while_finalizing { nullptr };
    bool* weak_to_self_was_null_while_finalizing { nullptr };

private:
    virtual void finalize() override
    {
        Base::finalize();
        *weak_cell_was_null_while_finalizing = !weak_cell;
        *weak_to_self_was_null_while_finalizing = !GC::Weak<FinalizerObservingCell> { *this };
    }
};

GC_DEFINE_ALLOCATOR(FinalizerObservingCell);

static GC::Heap& test_heap()
{
    static AK::NeverDestroyed<GC::Heap> heap([](auto&) { });
    return *heap;
}

static NEVER_INLINE void scrub_stack()
{
    u8 volatile filler[8 * KiB];
    for (size_t i = 0; i < sizeof(filler); ++i)
        filler[i] = 0;
}

TEST_SETUP
{
    GC::Heap::set_default_heap_for_testing(test_heap());
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

static bool possible_values_contain(GC::ConservativeHashMapBase const& container, GC::Cell* cell)
{
    bool found = false;
    auto target = bit_cast<FlatPtr>(cell);
    container.for_each_possible_value([&](FlatPtr value) {
        if (value == target)
            found = true;
    });
    return found;
}

static bool possible_values_contain(GC::ConservativeHashTableBase const& container, GC::Cell* cell)
{
    bool found = false;
    auto target = bit_cast<FlatPtr>(cell);
    container.for_each_possible_value([&](FlatPtr value) {
        if (value == target)
            found = true;
    });
    return found;
}

TEST_CASE(root_vector_reports_roots)
{
    auto& heap = test_heap();
    GC::RootVector<GC::Ref<TestCell>> vector;

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(activity_root_controls_cell_liveness)
{
    auto& heap = test_heap();
    GC::ActivityRoot root;
    GC::Weak<TestCell> weak_cell;

    {
        auto cell = heap.allocate<TestCell>();
        weak_cell = cell.ptr();
        root.take(*cell);
    }

    scrub_stack();
    heap.collect_garbage();
    EXPECT(weak_cell);

    root.release();
    scrub_stack();
    heap.collect_garbage(GC::Heap::CollectionType::CollectEverything);
    EXPECT(!weak_cell);
}

TEST_CASE(root_vector_ptr_reports_roots)
{
    auto& heap = test_heap();
    GC::RootVector<GC::Ptr<TestCell>> vector;

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
    GC::RootHashMap<int, GC::Ref<TestCell>> map;

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
    GC::RootHashMap<GC::Ref<TestCell>, int> map;

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
    GC::RootHashMap<GC::Ref<TestCell>, GC::Ref<TestCell>> map;

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
    GC::RootHashMap<int, GC::Ref<TestCell>> map;

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
    GC::RootVector<GC::Ref<TestCell>> vector;

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);
    vector.clear();

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);

    EXPECT_EQ(roots.size(), 0u);
}

TEST_CASE(root_vector_adopts_moved_vector)
{
    auto& heap = test_heap();
    Vector<GC::Ref<TestCell>> vector;

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);
    GC::RootVector<GC::Ref<TestCell>> root_vector { move(vector) };

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    root_vector.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(conservative_vector_adopts_moved_vector)
{
    auto& heap = test_heap();
    Vector<GC::Ref<TestCell>> vector;

    auto cell = heap.allocate<TestCell>();
    vector.append(cell);
    GC::ConservativeVector<GC::Ref<TestCell>> conservative_vector { move(vector) };

    EXPECT(possible_values_contain(conservative_vector, cell.ptr()));
}

TEST_CASE(conservative_vector_reports_possible_values)
{
    auto& heap = test_heap();
    GC::ConservativeVector<GC::Ref<TestCell>> vector;

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

TEST_CASE(root_hash_table_reports_roots)
{
    auto& heap = test_heap();
    GC::RootHashTable<GC::Ref<TestCell>> table;

    auto cell = heap.allocate<TestCell>();
    table.set(cell);

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    table.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

template<typename Container, typename AddCell, typename Transfer>
static void expect_transferred_container_keeps_cell_alive(AddCell add_cell, Transfer transfer)
{
    auto& heap = test_heap();
    GC::Weak<TestCell> weak_cell;
    Optional<Container> destination;

    {
        Container source;
        auto cell = heap.allocate<TestCell>();
        weak_cell = cell.ptr();
        add_cell(source, cell);
        transfer(destination, source);
    }

    scrub_stack();
    heap.collect_garbage();
    EXPECT(weak_cell);
}

TEST_CASE(root_hash_table_adopts_moved_hash_table)
{
    auto& heap = test_heap();
    HashTable<GC::Ref<TestCell>> table;

    auto cell = heap.allocate<TestCell>();
    table.set(cell);
    GC::RootHashTable<GC::Ref<TestCell>> root_table { move(table) };

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    root_table.gather_roots(roots);

    EXPECT(roots.contains(cell.ptr()));
    EXPECT_EQ(roots.size(), 1u);
}

TEST_CASE(copied_and_moved_root_hash_maps_keep_cells_alive)
{
    using Map = GC::RootHashMap<int, GC::Ref<TestCell>>;
    auto add_cell = [](Map& map, GC::Ref<TestCell> cell) { map.set(42, cell); };
    expect_transferred_container_keeps_cell_alive<Map>(add_cell, [](Optional<Map>& destination, Map& source) { destination = source; });
    expect_transferred_container_keeps_cell_alive<Map>(add_cell, [](Optional<Map>& destination, Map& source) { destination = move(source); });
    expect_transferred_container_keeps_cell_alive<Map>(add_cell, [](Optional<Map>& destination, Map& source) {
        destination.emplace();
        *destination = source;
    });
    expect_transferred_container_keeps_cell_alive<Map>(add_cell, [](Optional<Map>& destination, Map& source) {
        destination.emplace();
        *destination = move(source);
    });
}

TEST_CASE(copied_and_moved_root_hash_tables_keep_cells_alive)
{
    using Table = GC::RootHashTable<GC::Ref<TestCell>>;
    auto add_cell = [](Table& table, GC::Ref<TestCell> cell) { table.set(cell); };
    expect_transferred_container_keeps_cell_alive<Table>(add_cell, [](Optional<Table>& destination, Table& source) { destination = source; });
    expect_transferred_container_keeps_cell_alive<Table>(add_cell, [](Optional<Table>& destination, Table& source) { destination = move(source); });
    expect_transferred_container_keeps_cell_alive<Table>(add_cell, [](Optional<Table>& destination, Table& source) {
        destination.emplace();
        *destination = source;
    });
    expect_transferred_container_keeps_cell_alive<Table>(add_cell, [](Optional<Table>& destination, Table& source) {
        destination.emplace();
        *destination = move(source);
    });
}

TEST_CASE(empty_containers_report_no_roots)
{
    GC::RootVector<GC::Ref<TestCell>> vector;
    GC::RootHashTable<GC::Ref<TestCell>> table;
    GC::RootHashMap<int, GC::Ref<TestCell>> map;

    HashMap<GC::Cell*, GC::HeapRoot> roots;
    vector.gather_roots(roots);
    table.gather_roots(roots);
    map.gather_roots(roots);

    EXPECT_EQ(roots.size(), 0u);
}

TEST_CASE(conservative_hash_map_reports_possible_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<int, GC::Ref<TestCell>> map;

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);

    EXPECT(possible_values_contain(map, cell.ptr()));
}

TEST_CASE(cleared_conservative_hash_map_reports_no_stale_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<int, GC::Ref<TestCell>> map;

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);
    map.clear();

    EXPECT(!possible_values_contain(map, cell.ptr()));
}

TEST_CASE(removed_conservative_hash_map_entry_not_reported)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<int, GC::Ref<TestCell>> map;

    auto cell = heap.allocate<TestCell>();
    map.set(42, cell);
    map.remove(42);

    EXPECT(!possible_values_contain(map, cell.ptr()));
}

TEST_CASE(conservative_hash_map_key_reports_possible_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<GC::Ref<TestCell>, int> map;

    auto cell = heap.allocate<TestCell>();
    map.set(cell, 42);

    EXPECT(possible_values_contain(map, cell.ptr()));
}

TEST_CASE(removed_conservative_hash_map_key_not_reported)
{
    auto& heap = test_heap();
    GC::ConservativeHashMap<GC::Ref<TestCell>, int> map;

    auto cell = heap.allocate<TestCell>();
    map.set(cell, 42);
    map.remove(cell);

    EXPECT(!possible_values_contain(map, cell.ptr()));
}

TEST_CASE(weak_hash_map_non_cell_key_cell_value)
{
    auto& heap = test_heap();
    GC::WeakHashMap<int, TestCell> map;

    auto cell = heap.allocate<TestCell>();
    map.set(42, *cell);

    EXPECT(map.contains(42));
    EXPECT_EQ(map.get(42), cell.ptr());

    EXPECT(map.remove(42));
    EXPECT(!map.contains(42));
    EXPECT_EQ(map.get(42), static_cast<TestCell*>(nullptr));
}

TEST_CASE(weak_hash_map_cell_key_non_cell_value)
{
    auto& heap = test_heap();
    GC::WeakHashMap<TestCell, int> map;

    auto cell = heap.allocate<TestCell>();
    map.set(*cell, 7);

    EXPECT(map.contains(*cell));
    EXPECT_EQ(map.get(*cell).value(), 7);

    EXPECT(map.remove(*cell));
    EXPECT(!map.contains(*cell));
}

TEST_CASE(weak_hash_map_cell_key_and_cell_value)
{
    auto& heap = test_heap();
    GC::WeakHashMap<TestCell, TestCell> map;

    auto key = heap.allocate<TestCell>();
    auto value = heap.allocate<TestCell>();
    map.set(*key, *value);

    EXPECT(map.contains(*key));
    EXPECT_EQ(map.get(*key), value.ptr());
}

TEST_CASE(weak_hash_map_value_collection_clears_entry)
{
    auto& heap = test_heap();
    GC::WeakHashMap<int, TestCell> map;

    {
        auto cell = heap.allocate<TestCell>();
        map.set(1, *cell);
    }

    heap.collect_garbage(GC::Heap::CollectionType::CollectEverything);

    EXPECT_EQ(map.get(1), static_cast<TestCell*>(nullptr));
}

TEST_CASE(weak_hash_map_ensure_creates_missing_cell_value)
{
    auto& heap = test_heap();
    GC::WeakHashMap<int, TestCell> map;

    bool called = false;
    auto& value = map.ensure(1, [&] {
        called = true;
        return heap.allocate<TestCell>();
    });

    EXPECT(called);
    EXPECT_EQ(map.get(1), &value);
}

TEST_CASE(weak_hash_map_ensure_reuses_live_cell_value)
{
    auto& heap = test_heap();
    GC::WeakHashMap<int, TestCell> map;

    auto cell = heap.allocate<TestCell>();
    map.set(1, *cell);

    bool called = false;
    auto& value = map.ensure(1, [&] {
        called = true;
        return heap.allocate<TestCell>();
    });

    EXPECT(!called);
    EXPECT_EQ(&value, cell.ptr());
}

TEST_CASE(weak_hash_map_ensure_replaces_collected_cell_value)
{
    auto& heap = test_heap();
    GC::WeakHashMap<int, TestCell> map;

    {
        auto cell = heap.allocate<TestCell>();
        map.set(1, *cell);
    }

    heap.collect_garbage(GC::Heap::CollectionType::CollectEverything);
    EXPECT_EQ(map.get(1), static_cast<TestCell*>(nullptr));

    auto replacement = heap.allocate<TestCell>();
    bool called = false;
    auto& value = map.ensure(1, [&] {
        called = true;
        return replacement;
    });

    EXPECT(called);
    EXPECT_EQ(&value, replacement.ptr());
    EXPECT_EQ(map.get(1), replacement.ptr());
}

TEST_CASE(weak_hash_map_ensure_handles_non_cell_value)
{
    auto& heap = test_heap();
    GC::WeakHashMap<TestCell, int> map;

    auto key = heap.allocate<TestCell>();

    bool called = false;
    auto& value = map.ensure(*key, [&] {
        called = true;
        return 7;
    });

    EXPECT(called);
    EXPECT_EQ(value, 7);

    value = 9;
    called = false;
    auto& existing_value = map.ensure(*key, [&] {
        called = true;
        return 11;
    });

    EXPECT(!called);
    EXPECT_EQ(existing_value, 9);

    auto default_key = heap.allocate<TestCell>();
    auto& default_value = map.ensure(*default_key);
    EXPECT_EQ(default_value, 0);

    default_value = 13;
    EXPECT_EQ(map.get(*default_key).value(), 13);
}

TEST_CASE(conservative_hash_table_reports_possible_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashTable<GC::Ref<TestCell>> table;

    auto cell = heap.allocate<TestCell>();
    table.set(cell);

    EXPECT(possible_values_contain(table, cell.ptr()));
}

TEST_CASE(cleared_conservative_hash_table_reports_no_stale_values)
{
    auto& heap = test_heap();
    GC::ConservativeHashTable<GC::Ref<TestCell>> table;

    auto cell = heap.allocate<TestCell>();
    table.set(cell);
    table.clear();

    EXPECT(!possible_values_contain(table, cell.ptr()));
}

TEST_CASE(removed_conservative_hash_table_entry_not_reported)
{
    auto& heap = test_heap();
    GC::ConservativeHashTable<GC::Ref<TestCell>> table;

    auto cell = heap.allocate<TestCell>();
    table.set(cell);
    table.remove(cell);

    EXPECT(!possible_values_contain(table, cell.ptr()));
}

TEST_CASE(weak_pointers_to_dead_cells_are_null_while_finalizing)
{
    auto& heap = test_heap();
    bool weak_cell_was_null_while_finalizing = false;
    bool weak_to_self_was_null_while_finalizing = false;

    {
        auto cell = heap.allocate<TestCell>();
        auto observer = heap.allocate<FinalizerObservingCell>();
        observer->weak_cell = cell.ptr();
        observer->weak_cell_was_null_while_finalizing = &weak_cell_was_null_while_finalizing;
        observer->weak_to_self_was_null_while_finalizing = &weak_to_self_was_null_while_finalizing;
    }

    scrub_stack();
    heap.collect_garbage(GC::Heap::CollectionType::CollectEverything);
    EXPECT(weak_cell_was_null_while_finalizing);
    EXPECT(weak_to_self_was_null_while_finalizing);
}

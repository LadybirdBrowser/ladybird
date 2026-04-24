/*
 * Copyright (c) 2026-present, Marc Butler <marc@mailworks.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibTest/TestCase.h>

static constexpr bool PrintReports = false;

namespace TestGC {

static FlatPtr obscure_ptr(GC::Cell* cell)
{
    return ~bit_cast<FlatPtr>(cell);
}

class SingleEdgeCell : public GC::Cell {
    GC_CELL(SingleEdgeCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(SingleEdgeCell);

public:
    void set_edge(GC::Ptr<GC::Cell> other) { m_edge = other; }

    static AK::Vector<FlatPtr>& destroyed()
    {
        static AK::Vector<FlatPtr> s_destroyed;
        return s_destroyed;
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        if (m_edge)
            visitor.visit(m_edge);
    }

    virtual ~SingleEdgeCell() override
    {
        destroyed().append(obscure_ptr(this));
    }

private:
    GC::Ptr<GC::Cell> m_edge;

    SingleEdgeCell() = default;
};

}

GC_DEFINE_ALLOCATOR(TestGC::SingleEdgeCell);

using namespace TestGC;

TEST_CASE(heap_unused)
{
    SingleEdgeCell::destroyed().clear();
    GC::Heap heap([](auto&) { });
}

TEST_CASE(unrooted_cell)
{
    SingleEdgeCell::destroyed().clear();
    GC::Heap heap([](auto&) { });

    auto cell = heap.allocate<SingleEdgeCell>();
    FlatPtr cell_addr = obscure_ptr(cell);

    heap.collect_garbage(GC::Heap::CollectionType::CollectGarbageEmbedderRootsOnly, PrintReports);
    EXPECT_EQ(SingleEdgeCell::destroyed().size(), 1u);
    EXPECT(SingleEdgeCell::destroyed().contains_slow(cell_addr));
}

TEST_CASE(weak_ref_nulled_after_collection)
{
    SingleEdgeCell::destroyed().clear();
    GC::Heap heap([](auto&) { });

    GC::Weak<SingleEdgeCell> weak_ref { heap.allocate<SingleEdgeCell>() };
    EXPECT(weak_ref);
    heap.collect_garbage(GC::Heap::CollectionType::CollectGarbageEmbedderRootsOnly, PrintReports);
    EXPECT(!weak_ref);
}

TEST_CASE(basic_liveness)
{
    SingleEdgeCell::destroyed().clear();
    AK::Vector<GC::Cell*> test_roots;
    GC::Heap heap([&test_roots](HashMap<GC::Cell*, GC::HeapRoot>& roots) {
        for (auto&& cell : test_roots)
            roots.set(cell, { GC::HeapRoot::Type::Root, nullptr });
    });

    auto parent = heap.allocate<SingleEdgeCell>();
    test_roots.append(parent);
    auto child = heap.allocate<SingleEdgeCell>();
    parent->set_edge(child);

    heap.collect_garbage(GC::Heap::CollectionType::CollectGarbageEmbedderRootsOnly, PrintReports);
    EXPECT_EQ(SingleEdgeCell::destroyed().size(), 0u);
}

TEST_CASE(single_cell_cycle)
{
    SingleEdgeCell::destroyed().clear();
    GC::Heap heap([](auto&) { });

    auto cell_a = heap.allocate<SingleEdgeCell>();
    cell_a->set_edge(cell_a);

    heap.collect_garbage(GC::Heap::CollectionType::CollectGarbageEmbedderRootsOnly, PrintReports);
    EXPECT_EQ(SingleEdgeCell::destroyed().size(), 1u);
}

TEST_CASE(two_cell_cycle)
{
    SingleEdgeCell::destroyed().clear();
    GC::Heap heap([](auto&) { });

    auto cell_a = heap.allocate<SingleEdgeCell>();
    auto cell_b = heap.allocate<SingleEdgeCell>();
    cell_a->set_edge(cell_b);
    cell_b->set_edge(cell_a);

    heap.collect_garbage(GC::Heap::CollectionType::CollectGarbageEmbedderRootsOnly, PrintReports);
    EXPECT_EQ(SingleEdgeCell::destroyed().size(), 2u);
}

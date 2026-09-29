/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>

// expected-note@+1 {{'TestCell' is defined here}}
class TestCell : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

public:
    TestCell() = default;
};

GC_DEFINE_ALLOCATOR(TestCell);

class DerivedTestCell : public TestCell {
    GC_CELL(DerivedTestCell, TestCell);

public:
    DerivedTestCell()
        : TestCell()
    {
    }
};

struct NonCellWithCellMembers {
    // expected-error@+1 {{GC::Cell type TestCell must be allocated with GC::Heap::allocate(), not stored by value}}
    TestCell m_cell;
    // expected-error@+1 {{GC::Cell type TestCell must be allocated with GC::Heap::allocate(), not stored by value}}
    TestCell m_cells[2];
    GC::Ptr<TestCell> m_pointer;
};

class CellWithCellMember : public GC::Cell {
    GC_CELL(CellWithCellMember, GC::Cell);

    // expected-error@+1 {{GC::Cell type TestCell must be allocated with GC::Heap::allocate(), not stored by value}}
    TestCell m_cell;
};

// expected-error@+1 {{GC::Cell type TestCell must be allocated with GC::Heap::allocate()}}
TestCell g_cell;

void use_cell(TestCell const&);

void construct_cells(GC::Heap& heap, void* storage)
{
    // expected-error@+1 {{GC::Cell type TestCell must be allocated with GC::Heap::allocate()}}
    TestCell local_cell;
    use_cell(local_cell);

    // expected-error@+1 {{GC::Cell type DerivedTestCell must be allocated with GC::Heap::allocate()}}
    static DerivedTestCell static_cell;
    use_cell(static_cell);

    // expected-error@+1 {{GC::Cell type TestCell must be allocated with GC::Heap::allocate()}}
    use_cell(TestCell());

    // expected-error@+2 {{GC::Cell type TestCell must be allocated with GC::Heap::allocate()}}
    // expected-error@+1 {{'TestCell' is allocated with the system allocator. Add AK_ALLOC_WITH_KMALLOC to the class, or specialize AllocatedWithSystemAllocator or AllocatedWithCustomAllocator for it}}
    use_cell(*new TestCell);

    // expected-error@+1 {{GC::Cell type TestCell must be allocated with GC::Heap::allocate()}}
    use_cell(*new (storage) TestCell);

    GC::Ref<TestCell> heap_cell = heap.allocate<TestCell>();
    use_cell(heap_cell);
}

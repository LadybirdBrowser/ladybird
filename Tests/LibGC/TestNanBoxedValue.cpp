/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapRegion.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakInlines.h>
#include <LibTest/TestCase.h>

class TestCell final : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

    u8 m_padding[16] {};
};

GC_DEFINE_ALLOCATOR(TestCell);

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

class TestNanBox final : public GC::NanBoxedValue {
public:
    static TestNanBox from_payload(u64 payload)
    {
        TestNanBox value;
        value.m_value.encoded = GC::SHIFTED_IS_CELL_PATTERN | payload;
        return value;
    }

    static TestNanBox from_cell(GC::Cell const* cell)
    {
        return from_payload(GC::NanBoxedValue::encode_pointer_bits(cell));
    }
};

class TestNanBoxHolder final : public GC::Cell {
    GC_CELL(TestNanBoxHolder, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestNanBoxHolder);

public:
    void set(TestNanBox value) { m_value = value; }
    void clear() { m_value = {}; }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(ReadonlySpan<TestNanBox> { &m_value, 1 });
    }

    TestNanBox m_value;
    u8 m_padding[8] {};
};

GC_DEFINE_ALLOCATOR(TestNanBoxHolder);

TEST_CASE(cell_payload_round_trips_through_heap_region)
{
    auto cell = test_heap().allocate<TestCell>();
    auto value = TestNanBox::from_cell(cell.ptr());

    EXPECT(value.is_cell());
    EXPECT_EQ(value.extract_pointer<GC::Cell>(), cell.ptr());
}

TEST_CASE(cell_payload_masks_junk_high_bits)
{
    auto cell = test_heap().allocate<TestCell>();
    auto offset = GC::NanBoxedValue::encode_pointer_bits(cell.ptr());
    auto value = TestNanBox::from_payload(offset | 0x0000'fc00'0000'0000ULL);
    auto decoded = value.extract_pointer<GC::Cell>();
    auto base = js_heap_region_base;

    EXPECT(reinterpret_cast<FlatPtr>(decoded) >= base);
    EXPECT(reinterpret_cast<FlatPtr>(decoded) < base + GC::HEAP_REGION_SIZE);
    EXPECT_EQ(decoded, cell.ptr());
}

TEST_CASE(conservative_scanner_decodes_caged_value)
{
    auto& heap = test_heap();
    GC::Weak<TestCell> weak_cell;
    u64 volatile boxed_value = 0;

    {
        auto cell = heap.allocate<TestCell>();
        weak_cell = cell.ptr();
        boxed_value = GC::SHIFTED_IS_CELL_PATTERN
            | GC::NanBoxedValue::encode_pointer_bits(cell.ptr())
            | 0x0000'fc00'0000'0000ULL;
        EXPECT(boxed_value != 0);
    }

    scrub_stack();
    heap.collect_garbage();
    EXPECT(weak_cell);

    boxed_value = 0;
    scrub_stack();
    heap.collect_garbage(GC::Heap::CollectionType::CollectEverything);
    EXPECT(!weak_cell);
}

TEST_CASE(stale_cell_payload_is_not_traced)
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    heap.set_incremental_sweep_enabled(true);
    auto holder = GC::make_root(heap.allocate<TestNanBoxHolder>());
    GC::Weak<TestCell> target;

    {
        auto cell = heap.allocate<TestCell>();
        target = cell.ptr();
        holder->set(TestNanBox::from_cell(cell.ptr()));
        heap.uproot_cell(cell.ptr());
    }

    scrub_stack();
    heap.collect_garbage();
    EXPECT(!target);
    EXPECT(heap.is_incremental_sweep_active());

    heap.collect_garbage();
    holder->clear();
}

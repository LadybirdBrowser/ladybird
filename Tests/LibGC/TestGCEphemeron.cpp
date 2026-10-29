/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/Testing.h>
#include <LibGC/WeakContainer.h>
#include <LibGC/WeakInlines.h>
#include <LibTest/TestCase.h>

namespace {

class TestNanBox : public GC::NanBoxedValue {
public:
    static TestNanBox from_cell(GC::Cell& cell)
    {
        TestNanBox box;
        box.m_value.encoded = GC::SHIFTED_IS_CELL_PATTERN | GC::NanBoxedValue::encode_pointer_bits(&cell);
        return box;
    }

    static TestNanBox from_double(double value)
    {
        TestNanBox box;
        box.m_value.as_double = value;
        return box;
    }

    double as_double() const { return m_value.as_double; }
};

class TestCell final : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

public:
    GC::Ptr<GC::Cell> edge;

private:
    TestCell() = default;

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(edge);
    }
};

GC_DEFINE_ALLOCATOR(TestCell);

// Holds its values as ephemerons of their keys, and drops the entries whose key died, as WeakMap does.
class EphemeronTable final
    : public GC::Cell
    , public GC::WeakContainer {
    GC_CELL(EphemeronTable, GC::Cell);
    GC_DECLARE_ALLOCATOR(EphemeronTable);

public:
    struct Entry {
        GC::RawPtr<GC::Cell> key;
        TestNanBox value;
    };

    void add(GC::Cell& key, TestNanBox value) { entries.append({ &key, value }); }

    Vector<Entry> entries;

private:
    EphemeronTable()
        : GC::WeakContainer(heap())
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        for (auto& entry : entries)
            visitor.visit_ephemeron(*entry.key, entry.value);
    }

    virtual GC::Cell const& owner_cell(Badge<GC::Heap>) const override { return *this; }

    virtual void remove_dead_cells(Badge<GC::Heap>) override
    {
        entries.remove_all_matching([](auto const& entry) { return !entry.key->is_marked(); });
    }
};

GC_DEFINE_ALLOCATOR(EphemeronTable);

struct Weaks {
    GC::Weak<TestCell> key;
    GC::Weak<TestCell> value;
};

NEVER_INLINE Weaks add_value_referring_to_its_key(GC::Heap& heap, EphemeronTable& table)
{
    auto key = heap.allocate<TestCell>();
    auto value = heap.allocate<TestCell>();
    value->edge = key.ptr();
    table.add(*key, TestNanBox::from_cell(*value));
    return { key.ptr(), value.ptr() };
}

// The entry of the second key comes first, so marking visits it before the entry of the first key marks the second.
NEVER_INLINE Weaks add_chain_from(GC::Heap& heap, EphemeronTable& table, TestCell& first_key)
{
    auto second_key = heap.allocate<TestCell>();
    auto value = heap.allocate<TestCell>();
    table.add(*second_key, TestNanBox::from_cell(*value));
    table.add(first_key, TestNanBox::from_cell(*second_key));
    return { second_key.ptr(), value.ptr() };
}

NEVER_INLINE GC::Weak<TestCell> add_value_of_foreign_key(GC::Heap& heap, GC::Heap& foreign_heap, EphemeronTable& table)
{
    auto key = foreign_heap.allocate<TestCell>();
    auto value = heap.allocate<TestCell>();
    table.add(*key, TestNanBox::from_cell(*value));
    return value.ptr();
}

NEVER_INLINE void add_double_of_dead_key(GC::Heap& heap, EphemeronTable& table)
{
    table.add(*heap.allocate<TestCell>(), TestNanBox::from_double(2.5));
}

}

static NEVER_INLINE void value_referring_to_its_key_dies_with_it()
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    heap.set_incremental_sweep_enabled(false);
    auto table = GC::make_root(heap.allocate<EphemeronTable>());

    auto weaks = add_value_referring_to_its_key(heap, *table);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT(!weaks.key);
    EXPECT(!weaks.value);
    EXPECT(table->entries.is_empty());
}

static NEVER_INLINE void value_survives_when_its_key_is_marked_later()
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    heap.set_incremental_sweep_enabled(false);
    auto table = GC::make_root(heap.allocate<EphemeronTable>());
    auto first_key = GC::make_root(heap.allocate<TestCell>());

    auto weaks = add_chain_from(heap, *table, *first_key);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT(weaks.key);
    EXPECT(weaks.value);
    EXPECT_EQ(table->entries.size(), 2u);
}

static NEVER_INLINE void value_of_key_outside_domain_survives_local_collection()
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    GC::Heap foreign_heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    heap.set_incremental_sweep_enabled(false);
    foreign_heap.set_incremental_sweep_enabled(false);
    auto table = GC::make_root(heap.allocate<EphemeronTable>());

    auto value = add_value_of_foreign_key(heap, foreign_heap, *table);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    // NB: A collection of this heap alone never marks the key, so the table drops its entry.
    EXPECT(value);
    EXPECT(table->entries.is_empty());
}

static NEVER_INLINE void non_cell_value_is_ignored()
{
    GC::Heap heap([](auto&) { }, GC::Heap::BecomeProcessDefault::No);
    heap.set_incremental_sweep_enabled(false);
    auto table = GC::make_root(heap.allocate<EphemeronTable>());
    auto key = GC::make_root(heap.allocate<TestCell>());
    table->add(*key, TestNanBox::from_double(1.5));

    add_double_of_dead_key(heap, *table);
    GC::scrub_stack_for_testing();
    heap.collect_garbage();

    EXPECT_EQ(table->entries.size(), 1u);
    EXPECT_EQ(table->entries[0].value.as_double(), 1.5);
}

// Under ASan with detect_stack_use_after_return, a function's locals live on a fake stack, but its frame on the real
// stack still reserves room for them — room it never writes, so it keeps whatever earlier calls left there, and the
// conservative scan reads it. A stale pointer there can keep a dropped cell alive. So each case runs in a frame of its
// own, on stack that scrub_stack_for_testing() has just cleared.
TEST_CASE(value_referring_to_its_key_dies_with_it)
{
    GC::scrub_stack_for_testing();
    value_referring_to_its_key_dies_with_it();
}

TEST_CASE(value_survives_when_its_key_is_marked_later)
{
    GC::scrub_stack_for_testing();
    value_survives_when_its_key_is_marked_later();
}

TEST_CASE(value_of_key_outside_domain_survives_local_collection)
{
    GC::scrub_stack_for_testing();
    value_of_key_outside_domain_survives_local_collection();
}

TEST_CASE(non_cell_value_is_ignored)
{
    GC::scrub_stack_for_testing();
    non_cell_value_is_ignored();
}

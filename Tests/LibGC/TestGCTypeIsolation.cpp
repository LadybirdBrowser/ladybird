/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/HashTable.h>
#include <LibGC/CAPI.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/DeferGC.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/Testing.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

namespace {

Atomic<size_t> s_live_cells { 0 };

// Each test uses types of its own, so that it starts out with empty pools.
template<int Tag>
class TaggedCell final : public GC::Cell {
    GC_CELL(TaggedCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TaggedCell);

public:
    virtual ~TaggedCell() override { s_live_cells.fetch_sub(1); }

private:
    TaggedCell() { s_live_cells.fetch_add(1); }

    [[maybe_unused]] FlatPtr m_room_for_a_freelist_entry[2] {};
};

template<int Tag>
GC_DEFINE_ALLOCATOR(TaggedCell<Tag>);

using Blocks = HashTable<FlatPtr>;

static constexpr size_t BLOCK_COUNT = 64;

// Fills cells until they are in BLOCK_COUNT blocks, and returns those.
template<typename T>
NEVER_INLINE Blocks allocate_blocks(GC::Heap& heap)
{
    GC::DeferGC defer_gc { heap };
    Blocks blocks;
    while (blocks.size() < BLOCK_COUNT)
        blocks.set(bit_cast<FlatPtr>(GC::HeapBlock::from_cell(heap.allocate<T>().ptr())));
    return blocks;
}

bool have_a_block_in_common(Blocks const& first, Blocks const& second)
{
    for (auto block : first) {
        if (second.contains(block))
            return true;
    }
    return false;
}

template<typename Callback>
void run_on_a_new_thread(IGNORE_USE_IN_ESCAPING_LAMBDA Callback callback)
{
    auto thread = Threading::Thread::construct("TypeIsolationTest"sv, [&callback] {
        callback();
        return static_cast<intptr_t>(0);
    });
    thread->start();
    MUST(thread->join());
}

}

TEST_CASE(blocks_stay_with_their_type_after_their_heap_dies)
{
    using A = TaggedCell<0>;
    using B = TaggedCell<1>;

    Blocks blocks_of_a;
    run_on_a_new_thread([&] {
        GC::Heap heap([](auto&) { });
        blocks_of_a = allocate_blocks<A>(heap);
    });
    EXPECT_EQ(blocks_of_a.size(), BLOCK_COUNT);

#if defined(AK_OS_LINUX) && !defined(HAS_ADDRESS_SANITIZER)
    // The pages went back with the heap: a page that is still there holds the block's header.
    for (auto block : blocks_of_a) {
        FlatPtr first_word = *bit_cast<FlatPtr volatile*>(block);
        EXPECT_EQ(first_word, 0u);
    }
#endif

    run_on_a_new_thread([&] {
        GC::Heap heap([](auto&) { });
        EXPECT(!have_a_block_in_common(blocks_of_a, allocate_blocks<B>(heap)));

        // The new heap reuses the blocks of the type it needs, and so takes no others.
        auto blocks_of_a_again = allocate_blocks<A>(heap);
        EXPECT_EQ(blocks_of_a_again.size(), blocks_of_a.size());
        for (auto block : blocks_of_a_again)
            EXPECT(blocks_of_a.contains(block));
    });
}

TEST_CASE(blocks_stay_with_their_type_after_a_collection_frees_them)
{
    using A = TaggedCell<2>;
    using B = TaggedCell<3>;

    run_on_a_new_thread([] {
        GC::Heap heap([](auto&) { });
        heap.set_incremental_sweep_enabled(false);

        auto live_cells_before = s_live_cells.load();
        auto blocks_of_a = allocate_blocks<A>(heap);
        auto allocated_cells = s_live_cells.load() - live_cells_before;

        GC::scrub_stack_for_testing();
        heap.collect_garbage();
        // NB: A value left in a register can keep a cell, and its block, alive. Nearly all of them have to go, or
        //     this proves nothing.
        EXPECT(s_live_cells.load() - live_cells_before < allocated_cells / 8);

        EXPECT(!have_a_block_in_common(blocks_of_a, allocate_blocks<B>(heap)));

        auto blocks_of_a_again = allocate_blocks<A>(heap);
        for (auto block : blocks_of_a_again)
            EXPECT(blocks_of_a.contains(block));
    });
}

TEST_CASE(a_foreign_type_has_the_same_pool_in_every_heap)
{
    static GCCellTypeInfo const small_cell_info {
        .cell_size = 64,
        .alignment = 8,
        .kind = 0,
        .visit_edges = [](GCCell*, GCVisitor*) { },
        .finalize = nullptr,
        .destroy = nullptr,
        .external_memory_size = nullptr,
        .class_name = nullptr,
    };
    static GCCellTypeInfo const large_cell_info { [] {
        auto info = small_cell_info;
        info.cell_size = 128;
        return info;
    }() };
    static int first_type;
    static int second_type;

    auto pool_of = [](GCCellTypeInfo const& info, void const* foreign_type) {
        auto* allocator = gc_allocator_create(&info, "Foreign", 7, foreign_type);
        auto* pool = &reinterpret_cast<GC::CellAllocatorDescriptorBase*>(allocator)->block_allocator();
        if (foreign_type)
            gc_allocator_destroy(allocator);
        return pool;
    };

    auto* pool = pool_of(small_cell_info, &first_type);
    EXPECT_EQ(pool_of(small_cell_info, &first_type), pool);
    EXPECT_NE(pool_of(small_cell_info, &second_type), pool);
    EXPECT_NE(pool_of(large_cell_info, &first_type), pool);

    // NB: Allocators without a foreign type live as long as the process.
    EXPECT_NE(pool_of(small_cell_info, nullptr), pool_of(small_cell_info, nullptr));
}

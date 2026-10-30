/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/CompressedPointer.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Heap.h>
#include <LibGC/Testing.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakInlines.h>
#include <LibTest/TestCase.h>

namespace {

class TestCell final : public GC::Cell {
    GC_CELL(TestCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestCell);

    u8 m_padding[32] {};
};

GC_DEFINE_ALLOCATOR(TestCell);

static GC::Heap& test_heap()
{
    static AK::NeverDestroyed<GC::Heap> heap([](auto&) { });
    return *heap;
}

TEST_SETUP
{
    GC::Heap::set_default_heap_for_testing(test_heap());
}

TEST_CASE(compressed_pointer_round_trips)
{
    auto cell = test_heap().allocate<TestCell>();
    auto compressed = GC::compress(cell.ptr());

    EXPECT(compressed >= GC::MIN_COMPRESSED_CELL_POINTER);
    EXPECT_EQ(GC::decompress(compressed), reinterpret_cast<FlatPtr>(cell.ptr()));
    EXPECT_EQ(GC::decompress_nonnull(compressed), reinterpret_cast<FlatPtr>(cell.ptr()));
}

TEST_CASE(null_compresses_to_zero)
{
    EXPECT_EQ(GC::compress(nullptr), 0u);
    EXPECT_EQ(GC::decompress(0), 0u);
}

TEST_CASE(every_compressed_pointer_decodes_inside_the_heap_region)
{
    auto start = js_heap_region_base;
    for (u32 compressed : { 1u, GC::MIN_COMPRESSED_CELL_POINTER, 0x8000'0000u, 0xffff'ffffu }) {
        auto address = GC::decompress_nonnull(compressed);
        EXPECT(address >= start);
        EXPECT(address + sizeof(FlatPtr) <= start + GC::HEAP_REGION_SIZE);
    }
}

using MakeWord = u64 (*)(u32 compressed);

// Allocates a cell that nothing refers to but the word made from its compressed pointer.
static NEVER_INLINE void allocate_cell_into_word(GC::Weak<TestCell>& weak_cell, u64 volatile& word, MakeWord make_word)
{
    auto cell = test_heap().allocate<TestCell>();
    weak_cell = cell.ptr();
    word = make_word(GC::compress(cell.ptr()));
}

// Keeps a cell alive through the given word only, then drops the word and collects again.
static void expect_word_keeps_cell_alive(MakeWord make_word)
{
    auto& heap = test_heap();
    GC::Weak<TestCell> weak_cell;
    u64 volatile word = 0;
    allocate_cell_into_word(weak_cell, word, make_word);

    GC::scrub_stack_for_testing();
    heap.collect_garbage();
    EXPECT(weak_cell);

    word = 0;
    GC::scrub_stack_for_testing();
    heap.collect_garbage(GC::Heap::CollectionType::CollectEverything);
    EXPECT(!weak_cell);
}

TEST_CASE(conservative_scanner_finds_compressed_pointer_in_low_half)
{
    expect_word_keeps_cell_alive([](u32 compressed) -> u64 { return 0xdead'beef'0000'0000ULL | compressed; });
}

TEST_CASE(conservative_scanner_finds_compressed_pointer_in_high_half)
{
    expect_word_keeps_cell_alive([](u32 compressed) -> u64 { return (static_cast<u64>(compressed) << 32) | 0x1234; });
}

TEST_CASE(conservative_scanner_ignores_compressed_pointer_into_a_cell)
{
    auto& heap = test_heap();
    GC::Weak<TestCell> weak_cell;
    u64 volatile word = 0;
    // One unit past the cell's start points into the cell, which no compressed pointer does.
    allocate_cell_into_word(weak_cell, word, [](u32 compressed) -> u64 { return compressed + 1; });

    GC::scrub_stack_for_testing();
    heap.collect_garbage();
    EXPECT(!weak_cell);
    EXPECT(word != 0);
}

static NEVER_INLINE void allocate_cell_into_vector(GC::Weak<TestCell>& weak_cell, GC::ConservativeVector<u64>& words)
{
    auto cell = test_heap().allocate<TestCell>();
    weak_cell = cell.ptr();
    words.append(static_cast<u64>(GC::compress(cell.ptr())) << 32);
}

TEST_CASE(conservative_vector_keeps_compressed_pointer_alive)
{
    auto& heap = test_heap();
    GC::Weak<TestCell> weak_cell;
    GC::ConservativeVector<u64> words;
    allocate_cell_into_vector(weak_cell, words);

    GC::scrub_stack_for_testing();
    heap.collect_garbage();
    EXPECT(weak_cell);

    words.clear();
    GC::scrub_stack_for_testing();
    heap.collect_garbage();
    EXPECT(!weak_cell);
}

}

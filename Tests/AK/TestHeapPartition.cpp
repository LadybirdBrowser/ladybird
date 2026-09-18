/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>

// A struct freed from the general heap must never be handed back as ByteBuffer or Vector storage, or a dangling
// pointer to the struct would read bytes a web page chose. Free one block from the middle of a batch (so its page
// stays with the general heap) and check that no buffer allocation of the same size lands on it.

// Sanitizer builds use the system allocator, which has no partitions and quarantines freed blocks.
#if !(__has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__))

static constexpr size_t block_size = 64;
static constexpr size_t batch_size = 64;
static constexpr size_t attempts = 1 << 16;

struct FreedBlock {
    Vector<void*> live;
    void* freed { nullptr };
};

static FreedBlock free_one_general_block()
{
    FreedBlock result;
    for (size_t i = 0; i < batch_size; ++i)
        result.live.append(kmalloc(block_size));
    result.freed = result.live[batch_size / 2];
    kfree(result.freed);
    result.live[batch_size / 2] = nullptr;
    return result;
}

static void release(FreedBlock& block)
{
    for (auto* pointer : block.live) {
        if (pointer)
            kfree(pointer);
    }
}

TEST_CASE(general_block_is_reused_by_general_allocations)
{
    auto block = free_one_general_block();
    Vector<void*> reclaimed;
    bool found = false;
    for (size_t i = 0; i < attempts && !found; ++i) {
        reclaimed.append(kmalloc(block_size));
        found = reclaimed.last() == block.freed;
    }
    EXPECT(found);
    for (auto* pointer : reclaimed)
        kfree(pointer);
    release(block);
}

TEST_CASE(general_block_is_never_reused_by_byte_buffer_storage)
{
    auto block = free_one_general_block();
    Vector<ByteBuffer> buffers;
    for (size_t i = 0; i < attempts; ++i) {
        auto buffer = MUST(ByteBuffer::create_uninitialized(block_size));
        EXPECT_NE(static_cast<void*>(buffer.data()), block.freed);
        buffers.append(move(buffer));
    }
    release(block);
}

TEST_CASE(general_block_is_reused_by_pointer_vector_storage)
{
    auto block = free_one_general_block();
    Vector<Vector<void*>> vectors;
    bool found = false;
    for (size_t i = 0; i < attempts && !found; ++i) {
        Vector<void*> vector;
        vector.resize(block_size / sizeof(void*));
        found = static_cast<void*>(vector.data()) == block.freed;
        vectors.append(move(vector));
    }
    EXPECT(found);
    release(block);
}

TEST_CASE(general_block_is_never_reused_by_byte_vector_storage)
{
    auto block = free_one_general_block();
    Vector<Vector<u8>> vectors;
    for (size_t i = 0; i < attempts; ++i) {
        Vector<u8> vector;
        vector.resize(block_size);
        EXPECT_NE(static_cast<void*>(vector.data()), block.freed);
        vectors.append(move(vector));
    }
    release(block);
}

#endif

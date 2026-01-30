/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/SharedBufferStream.h>
#include <LibTest/TestCase.h>

TEST_CASE(shared_buffer_stream_smoke)
{
    // Create pool buffer.
    u32 block_size = 16;
    u32 block_count = 8;

    auto pool_buffer = MUST(Core::AnonymousBuffer::create_with_size(Core::SharedBufferStream::pool_buffer_size_bytes(block_size, block_count)));
    auto* header = reinterpret_cast<Core::SharedBufferStream::PoolHeader*>(pool_buffer.data<void>());
    ASSERT(header);

    __builtin_memset(header, 0, sizeof(*header));
    header->magic = Core::SharedBufferStream::s_pool_magic;
    header->version = Core::SharedBufferStream::s_pool_version;
    header->block_size = block_size;
    header->block_count = block_count;

    // Create rings.
    auto ready_ring = MUST(Core::SharedSingleProducerCircularBuffer::create(256));
    auto free_ring = MUST(Core::SharedSingleProducerCircularBuffer::create(256));

    // Seed free ring.
    for (u32 i = 0; i < block_count; ++i) {
        Core::SharedBufferStream::Descriptor desc { i, 0 };
        auto bytes = ReadonlyBytes { reinterpret_cast<u8 const*>(&desc), sizeof(desc) };
        EXPECT_EQ(free_ring.try_write(bytes), sizeof(desc));
    }

    auto producer = MUST(Core::SharedBufferStream::attach(pool_buffer, ready_ring.anonymous_buffer(), free_ring.anonymous_buffer()));
    auto consumer = MUST(Core::SharedBufferStream::attach(pool_buffer, ready_ring.anonymous_buffer(), free_ring.anonymous_buffer()));

    auto index = producer.try_acquire_block_index();
    EXPECT(index.has_value());

    auto block = producer.block_bytes(index.value());
    EXPECT_EQ(block.size(), block_size);
    __builtin_memset(block.data(), 0xAB, block.size());

    EXPECT(producer.try_submit_ready_block(index.value(), 12));

    auto ready = consumer.try_receive_ready_block();
    EXPECT(ready.has_value());
    EXPECT_EQ(ready->block_index, index.value());
    EXPECT_EQ(ready->used_size, 12u);

    auto payload = consumer.block_bytes(ready->block_index);
    EXPECT_EQ(payload.size(), block_size);
    EXPECT_EQ(payload[0], 0xAB);
    EXPECT_EQ(payload[11], 0xAB);

    EXPECT(consumer.try_release_block_index(ready->block_index));

    auto index2 = producer.try_acquire_block_index();
    EXPECT(index2.has_value());
}

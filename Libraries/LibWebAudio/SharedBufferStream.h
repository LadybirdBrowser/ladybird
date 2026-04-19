/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibAudioServer/SharedCircularBuffer.h>
#include <LibCore/AnonymousBuffer.h>

namespace Web::WebAudio {

using Core::AnonymousBuffer;

struct SharedBufferStreamCreateResult;

// A shared-memory buffer pool plus two rings (SharedCircularBuffer). The data payload lives in a
// shared pool and the clients exchange only small descriptors (index + size) over SPSC rings.
//
// - Producer gets a free block from free_ring, writes payload into pool block, then enqueues a
//   descriptor into ready_ring.
// - Consumer dequeues a descriptor from ready_ring, reads payload from pool block, then returns the
//   block index to free_ring.

class SharedBufferStream {
public:
    struct Descriptor {
        u32 block_index { 0 };
        u32 used_size { 0 };
    };

    struct PoolHeader {
        u32 magic;
        u32 block_size;
        u32 block_count;
        u32 reserved0;
        u32 reserved1;
        u32 reserved2;
        u32 reserved3;
    };

    static ErrorOr<SharedBufferStreamCreateResult> create(u32 block_size, u32 block_count);
    static ErrorOr<SharedBufferStream> attach(Core::AnonymousBuffer pool_buffer,
        Core::AnonymousBuffer ready_ring_buffer,
        Core::AnonymousBuffer free_ring_buffer);

    SharedBufferStream() = default;

    bool is_valid() const;
    u32 block_size() const;
    u32 block_count() const;
    Bytes block_bytes(u32 block_index);
    ReadonlyBytes block_bytes(u32 block_index) const;
    Audio::SharedCircularBuffer& ready_ring() { return m_ready_ring; }
    Audio::SharedCircularBuffer& free_ring() { return m_free_ring; }

    // Producer:
    Optional<u32> try_acquire_block_index();
    bool try_submit_ready_block(u32 block_index, u32 used_size);

    // Consumer:
    Optional<Descriptor> try_receive_ready_block();
    bool try_release_block_index(u32 block_index);

private:
    static size_t pool_buffer_size_bytes(u32 block_size, u32 block_count);
    static ErrorOr<bool> try_write_descriptor(Audio::SharedCircularBuffer& ring, Descriptor descriptor);
    static ErrorOr<Optional<Descriptor>> try_read_descriptor(Audio::SharedCircularBuffer& ring);

    Core::AnonymousBuffer m_pool_buffer;
    PoolHeader* m_pool_header { nullptr };
    u8* m_pool_data { nullptr };
    Audio::SharedCircularBuffer m_ready_ring;
    Audio::SharedCircularBuffer m_free_ring;
};

struct SharedBufferStreamBuffers {
    Core::AnonymousBuffer pool_buffer;
    Core::AnonymousBuffer ready_ring_buffer;
    Core::AnonymousBuffer free_ring_buffer;
};

struct SharedBufferStreamCreateResult {
    SharedBufferStream stream;
    SharedBufferStreamBuffers buffers;
};

} // namespace Web::WebAudio

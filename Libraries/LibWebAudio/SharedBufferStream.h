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

using AudioServer::SharedCircularBuffer;
using Core::AnonymousBuffer;

// A shared-memory buffer pool plus two SPSC descriptor rings.
//
// Intended use is high-rate data delivery where the data payload lives in a shared pool and the
// producer/consumer exchange only small descriptors (index + size) over SPSC rings.
//
// Typical topology:
// - Producer acquires a free block from free_ring, writes payload into pool block, then enqueues a
//   descriptor into ready_ring.
// - Consumer dequeues a descriptor from ready_ring, reads payload from pool block, then returns the
//   block index to free_ring.
//
// Both rings are LibAudioServer::SharedCircularBuffer instances, used as a byte ring with fixed
// descriptor-sized records.
class SharedBufferStream {
public:
    struct Descriptor {
        u32 block_index { 0 };
        u32 used_size { 0 };
    };

    static constexpr u32 s_pool_magic = 0x504F4F4Cu; // "POOL"
    static constexpr u32 s_pool_version = 1;

    struct PoolHeader {
        u32 magic;
        u32 version;
        u32 block_size;
        u32 block_count;
        u32 reserved0;
        u32 reserved1;
        u32 reserved2;
        u32 reserved3;
    };

    SharedBufferStream() = default;

    static size_t pool_buffer_size_bytes(u32 block_size, u32 block_count);

    static ErrorOr<SharedBufferStream> attach(AnonymousBuffer pool_buffer, AnonymousBuffer ready_ring_buffer, AnonymousBuffer free_ring_buffer);

    bool is_valid() const;

    u32 block_size() const;

    u32 block_count() const;

    Bytes block_bytes(u32 block_index);

    ReadonlyBytes block_bytes(u32 block_index) const;

    // Producer side: best-effort acquire a free block index.
    Optional<u32> try_acquire_block_index();

    // Producer side: best-effort enqueue a ready descriptor for the consumer.
    bool try_submit_ready_block(u32 block_index, u32 used_size);

    // Consumer side: best-effort dequeue a ready descriptor.
    Optional<Descriptor> try_receive_ready_block();

    // Consumer side: best-effort return a block index to the producer.
    bool try_release_block_index(u32 block_index);

    SharedCircularBuffer& ready_ring();
    SharedCircularBuffer& free_ring();

private:
    static ErrorOr<bool> try_write_descriptor(SharedCircularBuffer& ring, Descriptor descriptor);

    static ErrorOr<Optional<Descriptor>> try_read_descriptor(SharedCircularBuffer& ring);

    // Avoid pulling in ErrorOr helpers into all include sites; keep local.
    template<typename T>
    static T TRY_OR_IGNORE(ErrorOr<T> value_or_error)
    {
        if (value_or_error.is_error())
            return {};
        return value_or_error.release_value();
    }

    AnonymousBuffer m_pool_buffer;
    PoolHeader* m_pool_header { nullptr };
    u8* m_pool_data { nullptr };
    SharedCircularBuffer m_ready_ring;
    SharedCircularBuffer m_free_ring;
};

}

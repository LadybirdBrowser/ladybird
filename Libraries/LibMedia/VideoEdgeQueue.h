/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/SeqLock.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/SharedCircularQueue.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Export.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/VideoFrameHandle.h>

namespace Media {

// One decoded frame as it crosses the producer->consumer edge: a pool handle tagged with the seek ID it was decoded
// under, so the consumer can drain superseded frames after a seek.
struct VideoEdgeItem {
    VideoFrameHandle handle;
    u32 seek_id { 0 };
};
static_assert(IsTriviallyCopyable<VideoEdgeItem>);

// The coverage end of the latest frame the producer has enqueued, stamped with its seek ID. The consumer reads it to
// resolve a forward seek within the buffered lookahead without re-fetching.
struct VideoEdgeAvailableUpperBound {
    AK::Duration timestamp;
    u32 seek_id { 0 };
};
static_assert(IsTriviallyCopyable<VideoEdgeAvailableUpperBound>);

// The producer's pipeline status, stamped with the seek ID it reflects, so the consumer can ignore a status
// left over from before a seek (which would otherwise resolve the seek with no frame).
struct VideoEdgeStatus {
    PipelineStatus status { PipelineStatus::Pending };
    u32 seek_id { 0 };
};
static_assert(IsTriviallyCopyable<VideoEdgeStatus>);

// The shared-memory edge between a RemoteVideoSink (producer) and a RemoteVideoProducer (consumer).
// Holds an SPSC queue of video frame handles, the available upper bounding timestamp, and the status to produce when
// the queue runs dry.
class VideoEdgeQueue {
public:
    static constexpr size_t QUEUE_SIZE = 16;
    using Ring = Core::SharedSingleProducerCircularQueue<VideoEdgeItem, QUEUE_SIZE>;

    static ErrorOr<VideoEdgeQueue> create()
    {
        auto ring = TRY(Ring::create());
        auto header_buffer = TRY(Core::AnonymousBuffer::create_with_size(sizeof(Header)));
        auto* header = new (header_buffer.data<void>()) Header;
        return VideoEdgeQueue { move(ring), move(header_buffer), header };
    }

    static ErrorOr<VideoEdgeQueue> create(int ring_fd, Core::AnonymousBuffer header_buffer)
    {
        auto ring = TRY(Ring::create(ring_fd));
        if (!header_buffer.is_valid() || header_buffer.size() < sizeof(Header))
            return Error::from_string_literal("Invalid video edge header buffer");
        auto* header = reinterpret_cast<Header*>(header_buffer.data<void>());
        return VideoEdgeQueue { move(ring), move(header_buffer), header };
    }

    // Handed to the consumer at edge setup.
    int ring_fd() const { return m_ring.fd(); }
    Core::AnonymousBuffer const& header_buffer() const { return m_header_buffer; }

    // Producer side.
    ErrorOr<void> enqueue(VideoFrameHandle handle, u32 seek_id)
    {
        if (m_ring.enqueue({ handle, seek_id }).is_error())
            return Error::from_string_literal("Video edge queue is full");
        return {};
    }
    bool can_enqueue() const { return m_ring.can_enqueue(); }
    void set_status(PipelineStatus status, u32 seek_id) { m_header->status.store({ status, seek_id }); }
    void set_available_upper_bound(AK::Duration timestamp, u32 seek_id) { m_header->available_upper_bound.store({ timestamp, seek_id }); }

    // Consumer side. The status is consulted only when the ring is empty.
    Optional<VideoEdgeItem> peek() const { return m_ring.peek(); }
    void consume() { (void)m_ring.dequeue(); }
    Optional<VideoEdgeStatus> status() const { return m_header->status.read(); }
    Optional<VideoEdgeAvailableUpperBound> available_upper_bound() const { return m_header->available_upper_bound.read(); }

private:
    struct Header {
        SeqLock<VideoEdgeStatus> status;
        SeqLock<VideoEdgeAvailableUpperBound> available_upper_bound;
    };

    VideoEdgeQueue(Ring ring, Core::AnonymousBuffer header_buffer, Header* header)
        : m_ring(move(ring))
        , m_header_buffer(move(header_buffer))
        , m_header(header)
    {
    }

    Ring m_ring;
    Core::AnonymousBuffer m_header_buffer;
    Header* m_header { nullptr };
};

}

namespace IPC {

template<>
MEDIA_API ErrorOr<void> encode(Encoder&, Media::VideoEdgeQueue const&);

template<>
MEDIA_API ErrorOr<Media::VideoEdgeQueue> decode(Decoder&);

}

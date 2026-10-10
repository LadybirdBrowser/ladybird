/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/ThreadSafeWeakable.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/VideoDecoderClientEndpoint.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>
#include <LibMediaCodecs/VideoDecoderServerEndpoint.h>
#include <LibMediaCodecs/VideoFrameHandle.h>

namespace Media {

// The decoding end of the connection between a VideoDecoderServer and its client, hosting the decoders it is given in
// priority order. Each decoder the client creates runs on a thread of its own, reading the coded frames submitted to
// it out of a shared-memory ring. A decoded frame's slot stays held here until the client releases it.
class MEDIACODECS_API VideoDecoderServerConnection final
    : public IPC::ConnectionFromClient<VideoDecoderClientEndpoint, VideoDecoderServerEndpoint>
    , public ThreadSafeWeakable<VideoDecoderServerConnection> {
    C_OBJECT(VideoDecoderServerConnection);

public:
    virtual ~VideoDecoderServerConnection() override;

    Function<void()> on_death;

    // For tests of the client's recovery: the connection closes once its decoders have decoded this many frames, or
    // never if the count is zero.
    void close_after_decoded_frame_count(u32);

private:
    class DecoderSession;

    VideoDecoderServerConnection(NonnullOwnPtr<IPC::Transport>, ReadonlySpan<VideoDecoderRegistration> hosted_decoders);

    virtual void die() override;

    virtual Messages::VideoDecoderServer::DecoderCapabilitiesResponse decoder_capabilities(ParsedCodec codec) override;
    virtual void create_decoder(u64 decoder_id, u32 generation, CodecID codec_id, ByteBuffer codec_initialization_data) override;
    virtual void destroy_decoder(u64 decoder_id) override;
    virtual void replace_coded_data_ring(u64 decoder_id, Core::AnonymousBuffer ring) override;
    virtual void submit_coded_frame(u64 decoder_id, u32 generation, u64 ring_offset, u64 size, AK::Duration presentation_timestamp, AK::Duration decode_timestamp, AK::Duration duration, FrameFlags flags, Optional<ByteBuffer> new_codec_configuration, DecodeIntent intent, Optional<AK::Duration> output_target) override;
    virtual void signal_end_of_stream(u64 decoder_id, u32 generation) override;
    virtual void flush(u64 decoder_id, u32 generation) override;
    virtual void release_frame(VideoFramePoolID pool_id, u32 slot_index) override;

    // Called from the decoder threads.
    void hold_frame_until_released(NonnullRefPtr<VideoFrame>);
    bool should_close_after_decoding_frame();

    struct HeldSlot {
        VideoFramePoolID pool_id;
        u32 slot_index { 0 };

        bool operator==(HeldSlot const&) const = default;
    };
    struct HeldSlotTraits : public DefaultTraits<HeldSlot> {
        static unsigned hash(HeldSlot const& slot) { return pair_int_hash(u64_hash(slot.pool_id.value()), slot.slot_index); }
    };

    Core::EventLoop& m_event_loop;
    ReadonlySpan<VideoDecoderRegistration> const m_hosted_decoders;
    HashMap<u64, NonnullRefPtr<DecoderSession>> m_sessions;

    Mutex m_held_frames_mutex;
    HashMap<HeldSlot, NonnullRefPtr<VideoFrame>, HeldSlotTraits> m_held_frames;

    Atomic<u32> m_frames_until_close { 0 };
};

}

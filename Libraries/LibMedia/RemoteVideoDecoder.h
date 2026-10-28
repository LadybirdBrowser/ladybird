/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/kmalloc.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Export.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/CodecParameters.h>
#include <LibMediaCodecs/DecoderCapabilities.h>
#include <LibMediaCodecs/VideoDecoder.h>

namespace Media {

// Decodes in the VideoDecoderServer. Coded frames are written to a shared-memory ring and submitted without waiting,
// and the server answers each one once it has consumed it. Output is waited for once the current generation's frames
// the server has yet to answer reach the maximum in flight, unless the server waits for one of its slots to be
// released here, which is answered with TryAgain.
//
// If the server is lost, or answers nothing for too long while it is waited on, the next call reports
// NeedsResubmission, and the coded frames submitted after it, starting from a keyframe, go to a new server. A decoder
// that loses its server too many times answers NotImplemented instead, so that its stream falls back to the next
// decoder.
class MEDIA_API RemoteVideoDecoder final : public VideoDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr size_t MAXIMUM_FRAMES_IN_FLIGHT = 2;
    static constexpr u32 MAXIMUM_SERVER_LOSSES = 2;

    // The requester is called on the event loop of the thread installing it, and answers through the callback on that
    // thread, at once or later. Decoders are created and asked for capabilities on other threads, which wait for the
    // answer; the installing thread never asks, since it could not return to its event loop while waiting.
    using TransportCallback = Function<void(ErrorOr<NonnullOwnPtr<IPC::Transport>>)>;
    using TransportRequester = Function<void(TransportCallback)>;
    static void set_transport_requester(TransportRequester);

    static Optional<DecoderCapabilities> capabilities(ParsedCodec const&);
    static DecoderErrorOr<NonnullOwnPtr<RemoteVideoDecoder>> try_create(CodecID, ReadonlyBytes codec_initialization_data);

    virtual ~RemoteVideoDecoder() override;

    virtual void set_storage_freed_callback(Function<void()>) override;
    virtual void set_output_arrived_callback(Function<void()>) override;

    virtual DecoderErrorOr<void> receive_coded_data(CodedFrame const&, DecodeIntent) override;
    virtual void signal_end_of_stream() override;
    virtual DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_next_output(CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration> target = {}) override;
    virtual void flush() override;

    struct State;

private:
    RemoteVideoDecoder(CodecID, ByteBuffer codec_configuration, NonnullRefPtr<State>);

    DecoderErrorOr<void> start_server_decoder();
    DecoderErrorOr<u64> write_to_ring(ReadonlyBytes);
    DecoderError report_server_loss();

    CodecID const m_codec_id;
    ByteBuffer m_codec_configuration;
    NonnullRefPtr<State> m_state;

    // Only the caller's thread uses these.
    bool m_reached_end_of_stream { false };
    bool m_server_loss_was_reported { false };
    u32 m_server_loss_count { 0 };
    Optional<AK::Duration> m_output_target;
    Core::AnonymousBuffer m_ring;
    u64 m_ring_serial { 0 };
    u64 m_ring_write_offset { 0 };
};

}

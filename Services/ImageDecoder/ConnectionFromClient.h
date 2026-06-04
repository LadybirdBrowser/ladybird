/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <ImageDecoder/Forward.h>
#include <ImageDecoder/ImageDecoderClientEndpoint.h>
#include <ImageDecoder/ImageDecoderServerEndpoint.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/BitmapSequence.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibSync/Mutex.h>

namespace ImageDecoder {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override = default;

    virtual void die() override;

    struct DecodeResult {
        bool is_animated = false;
        u32 loop_count = 0;
        u32 frame_count = 0;
        Gfx::FloatPoint scale { 1, 1 };
        Gfx::BitmapSequence bitmaps;
        Vector<u32> durations;
        Gfx::ColorSpace color_profile;

        // Non-null for streaming animated sessions:
        RefPtr<Gfx::ImageDecoder> decoder;
        Core::AnonymousBuffer encoded_data;
    };

    struct AnimationSession : public AtomicRefCounted<AnimationSession> {
        Core::AnonymousBuffer encoded_data;
        RefPtr<Gfx::ImageDecoder> decoder;
        u32 frame_count { 0 };
        Sync::Mutex decoder_mutex;
    };

private:
    struct PendingJob : public AtomicRefCounted<PendingJob> {
        void cancel() { m_canceled.store(true, AK::MemoryOrder::memory_order_relaxed); }
        bool is_canceled() const { return m_canceled.load(AK::MemoryOrder::memory_order_relaxed); }

    private:
        Atomic<bool> m_canceled { false };
    };

    using FrameDecodeResult = Vector<Gfx::ImageFrameDescriptor>;

    explicit ConnectionFromClient(NonnullOwnPtr<IPC::Transport>);

    virtual void decode_image(Core::AnonymousBuffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type, i64 request_id) override;
    virtual void cancel_decoding(i64 request_id) override;
    virtual void request_animation_frames(i64 session_id, u32 start_frame_index, u32 count) override;
    virtual void stop_animation_decode(i64 session_id) override;
    virtual Messages::ImageDecoderServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;
    virtual Messages::ImageDecoderServer::InitTransportResponse init_transport(int peer_pid) override;

    ErrorOr<IPC::TransportHandle> connect_new_client();

    NonnullRefPtr<PendingJob> start_decode_image_job(i64 request_id, Core::AnonymousBuffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type);
    NonnullRefPtr<PendingJob> start_frame_decode_job(i64 session_id, NonnullRefPtr<AnimationSession>, u32 start_frame_index, u32 end_index);

    i64 m_next_session_id { 1 };
    HashMap<i64, NonnullRefPtr<PendingJob>> m_pending_jobs;
    HashMap<i64, NonnullRefPtr<AnimationSession>> m_animation_sessions;
    HashMap<i64, NonnullRefPtr<PendingJob>> m_pending_frame_jobs;
};

}

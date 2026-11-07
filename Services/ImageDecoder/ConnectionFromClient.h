/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Mutex.h>
#include <AK/RefCounted.h>
#include <ImageDecoder/Forward.h>
#include <ImageDecoder/ImageDecoderClientEndpoint.h>
#include <ImageDecoder/ImageDecoderServerEndpoint.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/BitmapSequence.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibIPC/ConnectionFromClient.h>

namespace ImageDecoder {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override = default;

    virtual void die() override;

    // The decoder exits once the Browser has let go of it and the last of the clients the Browser connected is gone.
    enum class IsHeldOpenByBrowser : u8 {
        No,
        Yes,
    };
    static void set_is_held_open_by_browser(IsHeldOpenByBrowser);

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
        Mutex decoder_mutex;
    };

    // A client and the further clients it connects, which share its animation sessions. Nothing here reaches the
    // clients of other groups, and the sessions go once the last client of the group is gone.
    struct ClientGroup : public RefCounted<ClientGroup> {
        HashTable<ConnectionFromClient*> clients;
        HashMap<i64, NonnullRefPtr<AnimationSession>> animation_sessions;
    };

private:
    struct PendingJob : public AtomicRefCounted<PendingJob> {
        void cancel() { m_canceled.store(true, AK::MemoryOrder::memory_order_relaxed); }
        bool is_canceled() const { return m_canceled.load(AK::MemoryOrder::memory_order_relaxed); }

    private:
        Atomic<bool> m_canceled { false };
    };

    using FrameDecodeResult = Vector<Gfx::ImageFrameDescriptor>;

    explicit ConnectionFromClient(NonnullOwnPtr<IPC::Transport>, RefPtr<ClientGroup> = nullptr);

    virtual void decode_image(Core::AnonymousBuffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type, i64 request_id) override;
    virtual Messages::ImageDecoderServer::DecodeFirstFrameSynchronouslyResponse decode_first_frame_synchronously(Core::AnonymousBuffer, Optional<ByteString> mime_type) override;
    virtual void cancel_decoding(i64 request_id) override;
    virtual void request_animation_frames(i64 session_id, u32 start_frame_index, u32 count) override;
    virtual void stop_animation_decode(i64 session_id) override;
    virtual void connect_frame_client(IPC::TransportHandle) override;

    NonnullRefPtr<PendingJob> start_decode_image_job(i64 request_id, Core::AnonymousBuffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type);
    NonnullRefPtr<PendingJob> start_frame_decode_job(i64 session_id, NonnullRefPtr<AnimationSession>, u32 start_frame_index, u32 end_index);

    // A frame client only plays the animations of the client that connected it, so it does not keep the decoder running.
    enum class IsFrameClient : u8 {
        No,
        Yes,
    };
    IsFrameClient m_is_frame_client { IsFrameClient::No };

    NonnullRefPtr<ClientGroup> m_group;
    HashMap<i64, NonnullRefPtr<PendingJob>> m_pending_jobs;
    HashMap<i64, NonnullRefPtr<PendingJob>> m_pending_frame_jobs;
};

}

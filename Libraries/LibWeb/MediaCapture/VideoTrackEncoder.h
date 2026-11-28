/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteBuffer.h>
#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibMedia/I420Picture.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibThreading/Forward.h>

namespace Web::MediaCapture {

// Encodes off the main thread, replacing any queued picture with the newest one to avoid a backlog.
class VideoTrackEncoder final : public AtomicRefCounted<VideoTrackEncoder> {
public:
    // Called on the thread the encoder was created on, with each frame in turn.
    using OnFrame = Function<void(Media::CodecID, bool is_key_frame, AK::Duration timestamp, ByteBuffer)>;

    // Supported codecs, in preference order.
    static ReadonlySpan<Media::CodecID> supported_codecs();

    static NonnullRefPtr<VideoTrackEncoder> create(Media::CodecID, OnFrame);
    ~VideoTrackEncoder();

    void encode(NonnullRefPtr<Media::I420Picture>, AK::Duration timestamp);
    void request_key_frame();
    // Stops frame delivery. Call on the creating thread before releasing the last reference.
    void stop();

private:
    VideoTrackEncoder(Media::CodecID, OnFrame);

    void run();

    Media::CodecID const m_codec;

    Mutex m_mutex;
    ConditionVariable m_wake { m_mutex };
    RefPtr<Media::I420Picture> m_pending_picture;
    AK::Duration m_pending_timestamp;
    bool m_key_frame_is_wanted { true };
    bool m_is_stopping { false };

    // Only used on the thread the encoder was created on.
    OnFrame m_on_frame;

    NonnullRefPtr<Core::WeakEventLoopReference> m_event_loop;
};

}

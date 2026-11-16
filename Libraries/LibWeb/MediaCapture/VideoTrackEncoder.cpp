/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Debug.h>
#include <AK/OwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Size.h>
#include <LibMedia/FFmpeg/FFmpegVideoEncoder.h>
#include <LibThreading/Thread.h>
#include <LibWeb/MediaCapture/VideoTrackEncoder.h>

namespace Web::MediaCapture {

// VP8 is one of the two codecs every WebRTC endpoint has to be able to receive, and the one of them that the bundled
// FFmpeg can encode.
// https://datatracker.ietf.org/doc/html/rfc7742#section-5
// FIXME: The other is H.264, which most calls settle on when they can. Its encoders are in the system's FFmpeg,
//        which this process doesn't load.
static constexpr Array SUPPORTED_CODECS { Media::CodecID::VP8 };

ReadonlySpan<Media::CodecID> VideoTrackEncoder::supported_codecs()
{
    return SUPPORTED_CODECS;
}

// Roughly what other browsers spend on a camera of the same size: 0.7 Mbit/s at 640x360, 2.5 Mbit/s at 1280x720.
static int bitrate_for_size(u32 width, u32 height)
{
    static constexpr u64 MINIMUM_BITRATE = 150'000;
    static constexpr u64 MAXIMUM_BITRATE = 4'000'000;
    auto pixels = static_cast<u64>(width) * height;
    return static_cast<int>(clamp(pixels * 3, MINIMUM_BITRATE, MAXIMUM_BITRATE));
}

NonnullRefPtr<VideoTrackEncoder> VideoTrackEncoder::create(Media::CodecID codec, OnFrame on_frame)
{
    auto encoder = adopt_ref(*new VideoTrackEncoder(codec, move(on_frame)));
    // The thread keeps the encoder alive until it is told to stop.
    auto thread = Threading::Thread::construct("VideoEncoder"sv, [encoder] {
        encoder->run();
        return 0;
    });
    thread->start();
    thread->detach();
    return encoder;
}

VideoTrackEncoder::VideoTrackEncoder(Media::CodecID codec, OnFrame on_frame)
    : m_codec(codec)
    , m_on_frame(move(on_frame))
    , m_event_loop(Core::EventLoop::current_weak())
{
}

VideoTrackEncoder::~VideoTrackEncoder() = default;

void VideoTrackEncoder::encode(NonnullRefPtr<Media::I420Picture> picture, AK::Duration timestamp)
{
    MutexLocker locker(m_mutex);
    m_pending_picture = move(picture);
    m_pending_timestamp = timestamp;
    m_wake.signal();
}

void VideoTrackEncoder::request_key_frame()
{
    MutexLocker locker(m_mutex);
    m_key_frame_is_wanted = true;
}

void VideoTrackEncoder::stop()
{
    m_on_frame = nullptr;
    MutexLocker locker(m_mutex);
    m_is_stopping = true;
    m_pending_picture = nullptr;
    m_wake.signal();
}

void VideoTrackEncoder::run()
{
    OwnPtr<Media::FFmpeg::FFmpegVideoEncoder> encoder;
    Optional<AK::Duration> last_timestamp;
    // A size that no encoder could be made for is passed over until the pictures come at another size.
    Optional<Gfx::IntSize> failed_size;
    auto size_of = [](Media::I420Picture const& picture) { return Gfx::IntSize { static_cast<int>(picture.width()), static_cast<int>(picture.height()) }; };

    while (true) {
        RefPtr<Media::I420Picture> picture;
        AK::Duration timestamp;
        bool as_key_frame = false;
        {
            MutexLocker locker(m_mutex);
            while (!m_pending_picture && !m_is_stopping)
                m_wake.wait();
            if (m_is_stopping)
                return;
            picture = move(m_pending_picture);
            timestamp = m_pending_timestamp;
            as_key_frame = m_key_frame_is_wanted;
            m_key_frame_is_wanted = false;
        }
        if (failed_size == size_of(*picture))
            continue;
        failed_size = {};

        // The encoder is made for one size of picture. Its first frame is a key frame without being asked.
        if (!encoder || encoder->width() != picture->width() || encoder->height() != picture->height()) {
            auto new_encoder = Media::FFmpeg::FFmpegVideoEncoder::try_create(m_codec, picture->width(), picture->height(), bitrate_for_size(picture->width(), picture->height()));
            if (new_encoder.is_error()) {
                dbgln("VideoTrackEncoder: No way to encode {}x{} video in {}: {}", picture->width(), picture->height(), m_codec, new_encoder.error().description());
                failed_size = size_of(*picture);
                encoder = nullptr;
                continue;
            }
            encoder = new_encoder.release_value();
        }

        // The encoder takes timestamps that increase, and two pictures can come in on the same tick of the clock.
        if (last_timestamp.has_value() && timestamp <= *last_timestamp)
            timestamp = *last_timestamp + AK::Duration::from_milliseconds(1);
        last_timestamp = timestamp;

        auto frame = encoder->encode(*picture, timestamp, as_key_frame);
        if (frame.is_error()) {
            dbgln("VideoTrackEncoder: Encoding failed: {}", frame.error().description());
            // After an encoding error, later frames may depend on an unusable frame.
            encoder = nullptr;
            continue;
        }
        if (!frame.value().has_value()) {
            // Retry any key frame request lost with the dropped picture.
            if (as_key_frame)
                request_key_frame();
            continue;
        }

        auto event_loop = m_event_loop->take();
        if (!event_loop)
            return;
        event_loop->deferred_invoke([self = NonnullRefPtr { *this }, timestamp, frame = frame.release_value().release_value()]() mutable {
            if (self->m_on_frame)
                self->m_on_frame(self->m_codec, frame.is_key_frame, timestamp, move(frame.data));
        });
    }
}

}

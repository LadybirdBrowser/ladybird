/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CameraServer/CaptureSession.h>
#include <CameraServer/FrameConversion.h>
#include <LibCore/EventLoop.h>

namespace CameraServer {

NonnullRefPtr<CaptureSession> CaptureSession::create(Callbacks callbacks, Camera::StreamRequest request)
{
    return adopt_ref(*new CaptureSession(move(callbacks), request));
}

CaptureSession::CaptureSession(Callbacks callbacks, Camera::StreamRequest request)
    : m_owner_loop(Core::EventLoop::current())
    , m_callbacks(move(callbacks))
    , m_request(request)
{
}

void CaptureSession::on_owner_loop(Function<void(CaptureSession&)> function)
{
    m_owner_loop.deferred_invoke([self = NonnullRefPtr(*this), function = move(function)] {
        if (!self->m_closed)
            function(*self);
    });
}

void CaptureSession::close()
{
    m_closed = true;
    // NB: This can run from inside one of the callbacks, which only clearing (not move-assigning) allows.
    m_callbacks.on_ready = nullptr;
    m_callbacks.on_frame = nullptr;
    m_callbacks.on_ended = nullptr;
}

void CaptureSession::release_frame(u32 buffer_index)
{
    MutexLocker locker(m_mutex);
    if (buffer_index < m_buffer_is_with_client.size())
        m_buffer_is_with_client[buffer_index] = false;
}

// Limits the pictures to the rate that was asked for, where the camera gives more. True if it does.
bool CaptureSession::update_frame_interval_while_locked(u32 camera_frame_rate)
{
    if (m_request.frame_rate == 0 || m_request.frame_rate >= camera_frame_rate) {
        m_frame_interval = {};
        return false;
    }
    m_frame_interval = AK::Duration::from_nanoseconds(1'000'000'000 / m_request.frame_rate);
    return true;
}

void CaptureSession::format_decided(u32 width, u32 height, u32 frame_rate)
{
    PictureSize size;
    auto buffers = [&]() -> ErrorOr<Vector<Core::AnonymousBuffer>> {
        MutexLocker locker(m_mutex);
        if (m_has_ended)
            return Vector<Core::AnonymousBuffer> {};
        // The client was told one size, and reads every picture at it.
        if (!m_buffers.is_empty()) {
            if (PictureSize { width, height } != m_camera_size)
                return Error::from_string_literal("The camera changed the size of its pictures");
            // NB: The camera may have changed its frame rate, which the limit follows. The client keeps the rate it was
            //     told at first, which is the most it is sent at.
            update_frame_interval_while_locked(frame_rate);
            return Vector<Core::AnonymousBuffer> {};
        }
        if (width == 0 || height == 0 || width > Camera::MAXIMUM_PICTURE_DIMENSION || height > Camera::MAXIMUM_PICTURE_DIMENSION)
            return Error::from_string_literal("The camera's pictures have an unusable size");

        PictureSize camera_size { width, height };
        size = output_size_for(camera_size, m_request);
        if (size != camera_size) {
            m_scaler = TRY(PictureScaler::create(camera_size, size));
            m_camera_picture = TRY(ByteBuffer::create_uninitialized(Camera::i420_size(width, height)));
        }
        if (update_frame_interval_while_locked(frame_rate))
            frame_rate = m_request.frame_rate;

        Vector<Core::AnonymousBuffer> buffers;
        for (size_t i = 0; i < BUFFER_COUNT; ++i)
            buffers.append(TRY(Core::AnonymousBuffer::create_with_size(Camera::i420_size(size.width, size.height))));
        m_buffers = buffers;
        m_buffer_is_with_client.resize(BUFFER_COUNT);
        m_camera_size = camera_size;
        m_size = size;
        return buffers;
    }();

    if (buffers.is_error()) {
        warnln("CameraServer: {}", buffers.error());
        ended();
        return;
    }
    if (buffers.value().is_empty())
        return;

    on_owner_loop([size, frame_rate, buffers = buffers.release_value()](auto& self) {
        if (self.m_callbacks.on_ready)
            self.m_callbacks.on_ready(size.width, size.height, frame_rate, buffers);
    });
}

void CaptureSession::frame_arrived(RawFrame const& frame)
{
    Optional<u32> buffer_index;
    {
        MutexLocker locker(m_mutex);
        if (m_has_ended || m_buffers.is_empty() || PictureSize { frame.width, frame.height } != m_camera_size)
            return;

        // Limit the frame rate while tolerating camera timing drift, which could otherwise drop every other frame.
        if (m_frame_interval.has_value()) {
            // NB: A quarter of an interval early is close enough, with pictures that never come exactly on time.
            auto tolerance = AK::Duration::from_nanoseconds(m_frame_interval->to_nanoseconds() / 4);
            if (m_next_frame_time.has_value() && frame.timestamp + tolerance < *m_next_frame_time)
                return;
            auto next = m_next_frame_time.value_or(frame.timestamp) + *m_frame_interval;
            // A camera that stalled is not owed the pictures it missed.
            m_next_frame_time = max(next, frame.timestamp);
        }
        for (u32 i = 0; i < m_buffer_is_with_client.size(); ++i) {
            if (m_buffer_is_with_client[i])
                continue;
            m_buffer_is_with_client[i] = true;
            buffer_index = i;
            break;
        }
        if (!buffer_index.has_value())
            return;

        // The buffer is nobody's but this thread's until the client hears of it.
        auto& buffer = m_buffers[*buffer_index];
        Bytes picture { buffer.data<u8>(), buffer.size() };
        if (convert_to_i420(frame, m_scaler.has_value() ? m_camera_picture.bytes() : picture).is_error()) {
            m_buffer_is_with_client[*buffer_index] = false;
            return;
        }
        if (m_scaler.has_value())
            m_scaler->scale(m_camera_picture, picture);
    }

    on_owner_loop([buffer_index = *buffer_index, timestamp = frame.timestamp](auto& self) {
        if (self.m_callbacks.on_frame)
            self.m_callbacks.on_frame(buffer_index, timestamp);
    });
}

void CaptureSession::ended()
{
    {
        MutexLocker locker(m_mutex);
        if (m_has_ended)
            return;
        m_has_ended = true;
    }
    on_owner_loop([](auto& self) {
        if (self.m_callbacks.on_ended)
            self.m_callbacks.on_ended();
    });
}

}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <CameraServer/CameraBackend.h>
#include <CameraServer/FrameConversion.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>

namespace CameraServer {

// One client's camera stream. The camera thread converts frames into buffers released by the client, dropping
// frames when none are free so a slow client affects only its own stream.
// Frames larger than requested are cropped and scaled down; excess frames are dropped to meet the requested rate.
class CaptureSession final : public CameraStreamListener {
public:
    static constexpr size_t BUFFER_COUNT = 3;

    // These run on the event loop of the thread that made the session, and never after close().
    struct Callbacks {
        Function<void(u32 width, u32 height, u32 frame_rate, Vector<Core::AnonymousBuffer> const&)> on_ready;
        Function<void(u32 buffer_index, AK::Duration timestamp)> on_frame;
        Function<void()> on_ended;
    };

    static NonnullRefPtr<CaptureSession> create(Callbacks, Camera::StreamRequest = {});

    void release_frame(u32 buffer_index);
    void close();

    virtual void format_decided(u32 width, u32 height, u32 frame_rate) override;
    virtual void frame_arrived(RawFrame const&) override;
    virtual void ended() override;

private:
    bool update_frame_interval_while_locked(u32 camera_frame_rate);

    CaptureSession(Callbacks, Camera::StreamRequest);

    void on_owner_loop(Function<void(CaptureSession&)>);

    Core::EventLoop& m_owner_loop;
    // Touched on the owner's thread only.
    Callbacks m_callbacks;
    bool m_closed { false };

    Mutex m_mutex;
    Vector<Core::AnonymousBuffer> m_buffers;
    Vector<bool> m_buffer_is_with_client;
    Camera::StreamRequest m_request;
    PictureSize m_camera_size;
    PictureSize m_size;
    // Where the two differ, the camera's picture as I420, and what makes the client's of it.
    ByteBuffer m_camera_picture;
    Optional<PictureScaler> m_scaler;
    // Where the camera is faster than the request, how far apart the pictures kept are, and when the next is due.
    Optional<AK::Duration> m_frame_interval;
    Optional<AK::Duration> m_next_frame_time;
    bool m_has_ended { false };
};

}

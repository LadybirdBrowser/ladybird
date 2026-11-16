/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCamera/StreamRequest.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Promise.h>

namespace Camera {

class ClientConnection;

// A picture from a camera as I420: the luma plane, then the two chroma planes at half the resolution each way, with no
// padding. The bytes are the CameraServer's to write again once the callback that was given them returns.
struct Frame {
    ReadonlyBytes i420;
    u32 width { 0 };
    u32 height { 0 };
    // On a clock that starts wherever the camera's does: only the difference of two means anything.
    AK::Duration timestamp;
};

// Pictures from a camera, which the CameraServer opened once the Browser let it. Lives on the thread that made it, where
// the pictures arrive as well.
class CaptureStream final : public RefCounted<CaptureStream> {
public:
    using CreatePromise = Core::Promise<NonnullRefPtr<CaptureStream>>;
    using FrameCallback = Function<void(Frame const&)>;

    // The device id is a dom_device_id from the device list, or empty for the default camera. The label names the page
    // the stream captures for. A stream the Browser refused is rejected with EPERM, and one whose camera would not open
    // with EIO. A process with no server to reach is refused, since only the CameraServer opens cameras.
    static NonnullRefPtr<CreatePromise> create(StringView device_id, u64 page_label, StreamRequest, FrameCallback);

    ~CaptureStream();

    // What the camera gives, which is the closest it could do to what was asked.
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    u32 frame_rate() const { return m_frame_rate; }

    // The server behind the stream is gone: nothing more is delivered, and the owner opens a new stream to carry on.
    Function<void()> on_capture_lost;
    // The camera is gone, or the permission to capture is: nothing more is delivered, and the owner ends.
    Function<void()> on_capture_ended;

private:
    friend class ClientConnection;

    CaptureStream(ClientConnection&, u64 stream_id, u32 width, u32 height, u32 frame_rate, Vector<Core::AnonymousBuffer>, FrameCallback);

    void deliver(u32 buffer_index, AK::Duration timestamp);
    // The connection calls these once, and forgets the stream as it does.
    void connection_lost();
    void capture_ended();

    RefPtr<ClientConnection> m_connection;
    u64 m_stream_id { 0 };
    u32 m_width { 0 };
    u32 m_height { 0 };
    u32 m_frame_rate { 0 };
    Vector<Core::AnonymousBuffer> m_buffers;
    FrameCallback m_callback;
};

}

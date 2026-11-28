/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <LibCamera/CaptureStream.h>
#include <LibWeb/Forward.h>
#include <LibWeb/MediaCapture/VideoPictureSource.h>

namespace Web::MediaCapture {

// A camera, as the source of the tracks getUserMedia() makes for it and of their clones. The CameraServer has the
// camera open for as long as one of them is live.
// https://w3c.github.io/mediacapture-main/#track-source
class CameraVideoSource final : public VideoPictureSource {
public:
    static NonnullRefPtr<CameraVideoSource> create();
    virtual ~CameraVideoSource() override;

    // Has the camera opened for the page, and says once how that went: EPERM where the Browser refused, EIO where the
    // camera would not open. The source keeps itself until then.
    void open(Page&, ByteString const& device_id, Camera::StreamRequest, Function<void(ErrorOr<void>)> on_opened);

    // What the camera gives, once it is open.
    u32 width() const { return m_stream ? m_stream->width() : 0; }
    u32 height() const { return m_stream ? m_stream->height() : 0; }
    u32 frame_rate() const { return m_stream ? m_stream->frame_rate() : 0; }

private:
    CameraVideoSource();

    virtual void did_lose_last_track() override;

    void deliver_frame(Camera::Frame const&);
    void camera_is_gone();

    RefPtr<Camera::CaptureStream> m_stream;
};

}

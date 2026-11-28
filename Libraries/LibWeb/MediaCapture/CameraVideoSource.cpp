/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/I420Picture.h>
#include <LibWeb/MediaCapture/CameraVideoSource.h>
#include <LibWeb/Page/Page.h>

namespace Web::MediaCapture {

NonnullRefPtr<CameraVideoSource> CameraVideoSource::create()
{
    return adopt_ref(*new CameraVideoSource);
}

CameraVideoSource::CameraVideoSource() = default;
CameraVideoSource::~CameraVideoSource() = default;

void CameraVideoSource::open(Page& page, ByteString const& device_id, Camera::StreamRequest request, Function<void(ErrorOr<void>)> on_opened)
{
    // NB: The stream is this source's alone, so its pictures stop coming before the source is gone.
    auto promise = page.client().create_camera_capture_stream(device_id, request, [this](Camera::Frame const& frame) {
        deliver_frame(frame);
    });

    struct Opening : public RefCounted<Opening> {
        Function<void(ErrorOr<void>)> on_opened;
    };
    auto opening = adopt_ref(*new Opening);
    opening->on_opened = move(on_opened);

    promise->when_resolved([self = NonnullRefPtr(*this), opening](NonnullRefPtr<Camera::CaptureStream>& stream) {
        self->m_stream = stream;
        // FIXME: Open the camera again when only its server is gone, as the microphone's tracks do.
        stream->on_capture_lost = [self = self.ptr()] { self->camera_is_gone(); };
        stream->on_capture_ended = [self = self.ptr()] { self->camera_is_gone(); };
        opening->on_opened({});
    });
    promise->when_rejected([self = NonnullRefPtr(*this), opening](Error& error) {
        opening->on_opened(Error::copy(error));
    });
}

void CameraVideoSource::deliver_frame(Camera::Frame const& frame)
{
    if (!has_tracks())
        return;
    auto picture = Media::I420Picture::create(frame.width, frame.height);
    if (picture.is_error())
        return;

    // The frame is laid out as the picture is: the luma, then the two planes of chroma, with no padding.
    auto y_size = picture.value()->y().size();
    auto chroma_size = picture.value()->u().size();
    if (frame.i420.size() < y_size + 2 * chroma_size)
        return;
    frame.i420.slice(0, y_size).copy_to(picture.value()->y());
    frame.i420.slice(y_size, chroma_size).copy_to(picture.value()->u());
    frame.i420.slice(y_size + chroma_size, chroma_size).copy_to(picture.value()->v());
    deliver(picture.release_value());
}

void CameraVideoSource::camera_is_gone()
{
    // Letting go of the stream can be the last thing that kept this source.
    NonnullRefPtr protector = *this;
    m_stream = nullptr;
    end_tracks_for_lost_source();
}

void CameraVideoSource::did_lose_last_track()
{
    // Nothing looks through the camera any more, so the server closes it, and the Browser's indicator goes out.
    m_stream = nullptr;
}

}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibWeb/Bindings/CanvasCaptureMediaStreamTrack.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/MediaCapture/VideoPictureSource.h>

namespace Web::MediaCapture {

// What one call to captureStream() captures: the bitmap of a canvas, as it changes.
// https://w3c.github.io/mediacapture-fromelement/#html-canvas-element-media-capture-extensions
class CanvasCaptureSource final : public VideoPictureSource {
public:
    static NonnullRefPtr<CanvasCaptureSource> create(HTML::HTMLCanvasElement&, Optional<double> frame_request_rate);
    virtual ~CanvasCaptureSource() override;

    void canvas_content_changed();
    // Captures the canvas as it is, whether it has changed or not.
    void request_frame();

private:
    CanvasCaptureSource(HTML::HTMLCanvasElement&, Optional<double> frame_request_rate);

    void schedule_capture();
    void capture();
    void set_tracks_muted(HTML::HTMLCanvasElement&, bool muted);

    GC::Weak<HTML::HTMLCanvasElement> m_canvas;
    Optional<double> m_frame_request_rate;
    RefPtr<Core::Timer> m_timer;
    Optional<MonotonicTime> m_last_capture_time;
    // Set while the canvas is not origin-clean, which mutes its tracks.
    bool m_tracks_are_muted { false };
};

// https://w3c.github.io/mediacapture-fromelement/#the-canvascapturemediastreamtrack
class CanvasCaptureMediaStreamTrack final : public MediaStreamTrack {
    WEB_WRAPPABLE(CanvasCaptureMediaStreamTrack, MediaStreamTrack);
    GC_DECLARE_ALLOCATOR(CanvasCaptureMediaStreamTrack);

public:
    static GC::Ref<CanvasCaptureMediaStreamTrack> create(HTML::HTMLCanvasElement&, NonnullRefPtr<CanvasCaptureSource>);
    virtual ~CanvasCaptureMediaStreamTrack() override;

    GC::Ref<HTML::HTMLCanvasElement> canvas() const { return m_canvas; }
    void request_frame();

private:
    CanvasCaptureMediaStreamTrack(HTML::HTMLCanvasElement&, NonnullRefPtr<CanvasCaptureSource>);

    virtual GC::Ref<MediaStreamTrack> allocate_clone() const override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::HTMLCanvasElement> m_canvas;
    NonnullRefPtr<CanvasCaptureSource> m_source;
};

}

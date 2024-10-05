/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Forward.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

struct VideoFrame {
    RefPtr<Gfx::Bitmap> frame;
    double position { 0.0 };
};

class HTMLVideoElement final : public HTMLMediaElement {
    WEB_PLATFORM_OBJECT(HTMLVideoElement, HTMLMediaElement);
    JS_DECLARE_ALLOCATOR(HTMLVideoElement);

public:
    virtual ~HTMLVideoElement() override;

    Layout::VideoBox* layout_node();
    Layout::VideoBox const* layout_node() const;

    void set_video_width(u32 video_width) { m_video_width = video_width; }
    u32 video_width() const;

    void set_video_height(u32 video_height) { m_video_height = video_height; }
    u32 video_height() const;

    void set_video_track(JS::GCPtr<VideoTrack>);

    void set_current_frame(Badge<VideoTrack>, RefPtr<Gfx::Bitmap> frame, double position);
    VideoFrame const& current_frame() const { return m_current_frame; }
    RefPtr<Gfx::Bitmap> const& poster_frame() const { return m_poster_frame; }

    // FIXME: This is a hack for images used as CanvasImageSource. Do something more elegant.
    RefPtr<Gfx::Bitmap> bitmap() const
    {
        return current_frame().frame;
    }

private:
    HTMLVideoElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    // https://html.spec.whatwg.org/multipage/media.html#the-video-element:dimension-attributes
    virtual bool supports_dimension_attributes() const override { return true; }

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>) override;

    virtual void on_playing() override;
    virtual void on_paused() override;
    virtual void on_seek(double, MediaSeekMode) override;

    WebIDL::ExceptionOr<void> determine_element_poster_frame(Optional<String> const& poster);

    JS::GCPtr<HTML::VideoTrack> m_video_track;
    VideoFrame m_current_frame;
    RefPtr<Gfx::Bitmap> m_poster_frame;

    u32 m_video_width { 0 };
    u32 m_video_height { 0 };

    JS::GCPtr<Fetch::Infrastructure::FetchController> m_fetch_controller;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;
};

}

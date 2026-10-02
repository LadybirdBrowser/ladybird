/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/DecodedImageFrame.h>
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
    WEB_WRAPPABLE(HTMLVideoElement, HTMLMediaElement);
    GC_DECLARE_ALLOCATOR(HTMLVideoElement);

public:
    virtual ~HTMLVideoElement() override;

    void set_intrinsic_video_dimensions(Optional<Gfx::Size<u32>>);
    u32 video_width() const;
    u32 video_height() const;

    virtual void update_natural_dimensions() override;
    Optional<Gfx::Size<u32>> natural_media_size() const;
    Optional<CSSPixelSize> natural_element_size() const;

    Optional<Gfx::DecodedImageFrame> const& poster_frame() const { return m_poster_frame; }

    // https://html.spec.whatwg.org/multipage/media.html#the-video-element:the-video-element-7
    // NB: We combine the values of...
    //      - The last frame of the video to have been rendered
    //      - The frame of video corresponding to the current playback position
    //     ...into the value of VideoFrame below, as the playback system itself implements
    //     the details of the selection of a video frame to match the specification in this
    //     respect.
    enum class Representation : u8 {
        VideoFrame,
        FirstVideoFrame,
        PosterFrame,
        TransparentBlack,
    };
    Representation current_representation() const;

    Optional<Gfx::DecodedImageFrame> current_decoded_image_frame() const;

    GC::Ref<WebIDL::Promise> request_picture_in_picture();

    GC::Ptr<PictureInPicture::PictureInPictureWindow> picture_in_picture_window() const { return m_picture_in_picture_window; }
    void set_picture_in_picture_window(GC::Ptr<PictureInPicture::PictureInPictureWindow> window) { m_picture_in_picture_window = window; }

    bool has_pending_picture_in_picture_promise(WebIDL::Promise const&) const;
    bool take_pending_picture_in_picture_promise(WebIDL::Promise const&);

    WebIDL::CallbackType* onenterpictureinpicture();
    void set_onenterpictureinpicture(WebIDL::CallbackType*);
    WebIDL::CallbackType* onleavepictureinpicture();
    void set_onleavepictureinpicture(WebIDL::CallbackType*);

private:
    HTMLVideoElement(DOM::Document&, DOM::QualifiedName);
    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void adopted_from(DOM::Document&) override;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    // https://html.spec.whatwg.org/multipage/media.html#the-video-element:dimension-attributes
    virtual bool supports_dimension_attributes() const override { return true; }

    virtual bool is_html_video_element() const override { return true; }

    virtual CSS::ElementBoxKind box_kind() const override;

    WebIDL::ExceptionOr<void> determine_element_poster_frame(Optional<Utf16String> const& poster);
    void run_disable_picture_in_picture_steps();

    GC::Ptr<HTML::VideoTrack> m_video_track;
    VideoFrame m_current_frame;
    Optional<Gfx::DecodedImageFrame> m_poster_frame;

    Optional<Gfx::Size<u32>> m_intrinsic_video_dimensions;
    Optional<CSSPixelSize> m_natural_dimensions;

    GC::Ptr<Fetch::Infrastructure::FetchController> m_fetch_controller;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;

    GC::Ptr<PictureInPicture::PictureInPictureWindow> m_picture_in_picture_window;
    Vector<GC::Ref<WebIDL::Promise>> m_pending_picture_in_picture_promises;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLVideoElement>() const { return is_html_video_element(); }

}

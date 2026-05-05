/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/Bindings/HTMLVideoElement.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/VideoTrack.h>
#include <LibWeb/HTML/VideoTrackList.h>
#include <LibWeb/Layout/VideoBox.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLVideoElement);

HTMLVideoElement::HTMLVideoElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLMediaElement(document, move(qualified_name))
{
}

HTMLVideoElement::~HTMLVideoElement() = default;

void HTMLVideoElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLVideoElement);
    Base::initialize(realm);
}

void HTMLVideoElement::finalize()
{
    Base::finalize();
}

void HTMLVideoElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_video_track);
    visitor.visit(m_fetch_controller);
}

void HTMLVideoElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::poster) {
        determine_element_poster_frame(value).release_value_but_fixme_should_propagate_errors();
    }
}

GC::Ptr<Layout::Node> HTMLVideoElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::VideoBox>(document(), *this, style);
}

Layout::VideoBox* HTMLVideoElement::layout_node()
{
    return static_cast<Layout::VideoBox*>(Node::layout_node());
}

Layout::VideoBox const* HTMLVideoElement::layout_node() const
{
    return static_cast<Layout::VideoBox const*>(Node::layout_node());
}

void HTMLVideoElement::set_intrinsic_video_dimensions(Optional<Gfx::Size<u32>> dimensions)
{
    if (m_intrinsic_video_dimensions == dimensions)
        return;
    m_intrinsic_video_dimensions = dimensions;

    // Whenever the natural width or natural height of the video changes (including, for example, because the
    // selected video track was changed), if the element's readyState attribute is not HAVE_NOTHING,
    // AD-HOC: We also use this to set the resolution in the steps that advance the ready state to HAVE_METADATA,
    //         otherwise we could assert the readyState condition, since any other calls should already have metadata.
    //         Also, the spec is not explicit on whether to fire the event when the element is emptied, but the check
    //         for HAVE_NOTHING implies that it should not fire in that case. Therefore, skip the event if the
    //         dimensions are not available. This matches other browsers.
    if (dimensions.has_value()) {
        // the user agent must queue a media element task given the media element to fire an event named resize at the media element.
        queue_a_media_element_task([this] {
            dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::resize));
        });
    }

    update_natural_dimensions();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-video-videowidth
u32 HTMLVideoElement::video_width() const
{
    // The videoWidth IDL attribute must return the intrinsic width of the video in CSS pixels. The videoHeight IDL
    // attribute must return the intrinsic height of the video in CSS pixels. If the element's readyState attribute
    // is HAVE_NOTHING, then the attributes must return 0.
    if (ready_state() == ReadyState::HaveNothing)
        return 0;
    if (m_intrinsic_video_dimensions.has_value())
        return m_intrinsic_video_dimensions->width();
    // AD-HOC: If the natural dimensions are not available, return 0. The non-normative text says that it should
    //         return 0 if the dimensions are not known. The HAVE_NOTHING check is likely assumed to cover this
    //         possibility.
    return 0;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-video-videoheight
u32 HTMLVideoElement::video_height() const
{
    // The videoWidth IDL attribute must return the intrinsic width of the video in CSS pixels. The videoHeight IDL
    // attribute must return the intrinsic height of the video in CSS pixels. If the element's readyState attribute
    // is HAVE_NOTHING, then the attributes must return 0.
    if (ready_state() == ReadyState::HaveNothing)
        return 0;
    if (m_intrinsic_video_dimensions.has_value())
        return m_intrinsic_video_dimensions->height();
    // AD-HOC: If the natural dimensions is not available, return 0. The non-normative text says that it should
    //         return 0 if the dimensions are not known. The HAVE_NOTHING check is likely assumed to cover this
    //         possibility.
    return 0;
}

void HTMLVideoElement::update_intrinsic_video_dimensions()
{
    if (selected_video_track_sink() == nullptr) {
        set_intrinsic_video_dimensions({});
        return;
    }

    auto current_frame = selected_video_track_sink()->current_frame();
    if (current_frame == nullptr)
        return;

    auto current_frame_size = current_frame->size();
    if (current_frame_size == m_intrinsic_video_dimensions)
        return;
    set_intrinsic_video_dimensions(current_frame_size);
}

void HTMLVideoElement::update_natural_dimensions()
{
    // https://html.spec.whatwg.org/multipage/media.html#concept-video-intrinsic-width
    // The natural width of a video element's playback area is the natural width of the poster frame, if that is
    // available and the element currently represents its poster frame; otherwise, it is the natural width of the video
    // resource, if that is available; otherwise the natural width is missing.

    // The natural height of a video element's playback area is the natural height of the poster frame, if that is
    // available and the element currently represents its poster frame; otherwise it is the natural height of the video
    // resource, if that is available; otherwise the natural height is missing.
    auto natural_dimensions = m_intrinsic_video_dimensions.map([](Gfx::Size<u32> size) { return size.to_type<CSSPixels>(); });

    if (current_representation() == Representation::PosterFrame && m_poster_frame)
        natural_dimensions = m_poster_frame->size().to_type<CSSPixels>();

    if (natural_dimensions == m_natural_dimensiosn)
        return;

    set_needs_layout_update(DOM::SetNeedsLayoutReason::HTMLVideoElementNaturalDimensionsChanged);
    m_natural_dimensiosn = natural_dimensions;
}

Optional<Gfx::Size<u32>> HTMLVideoElement::natural_media_size() const
{
    return m_intrinsic_video_dimensions;
}

Optional<CSSPixelSize> HTMLVideoElement::natural_element_size() const
{
    return m_natural_dimensiosn;
}

// https://html.spec.whatwg.org/multipage/media.html#attr-video-poster
WebIDL::ExceptionOr<void> HTMLVideoElement::determine_element_poster_frame(Optional<String> const& poster)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. If there is an existing instance of this algorithm running for this video element, abort that instance of
    //    this algorithm without changing the poster frame.
    if (m_fetch_controller)
        m_fetch_controller->stop_fetch();

    static constexpr auto finalize = [](HTMLVideoElement& self, RefPtr<Gfx::Bitmap> poster_frame) {
        self.m_poster_frame = move(poster_frame);
        self.m_load_event_delayer.clear();
        self.m_fetch_controller = nullptr;
        self.update_natural_dimensions();
    };

    // 2. If the poster attribute's value is the empty string or if the attribute is absent, then there is no poster
    //    frame; return.
    if (!poster.has_value() || poster->is_empty()) {
        finalize(*this, nullptr);
        return {};
    }

    // 3. Let url be the result of encoding-parsing a URL given the poster attribute's value, relative to the element's node document.
    auto url_record = document().encoding_parse_url(*poster);

    // 4. If url is failure, then return.
    if (!url_record.has_value()) {
        finalize(*this, nullptr);
        return {};
    }

    // 5. Let request be a new request whose URL is the resulting URL record, client is the element's node document's
    //    relevant settings object, destination is "image", initiator type is "video", credentials mode is "include",
    //    and whose use-URL-credentials flag is set.
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(url_record.release_value());
    request->set_client(&document().relevant_settings_object());
    request->set_destination(Fetch::Infrastructure::Request::Destination::Image);
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Video);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);
    request->set_use_url_credentials(true);

    // 6. Fetch request. This must delay the load event of the element's node document.
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    m_load_event_delayer.emplace(document());

    // 7. If an image is thus obtained, the poster frame is that image. Otherwise, there is no poster frame.
    fetch_algorithms_input.process_response = [weak_self = GC::Weak(*this)](auto response) {
        if (!weak_self)
            return;
        auto& self = *weak_self;
        auto& realm = self.realm();
        auto& global = self.document().realm().global_object();

        if (response->is_network_error()) {
            finalize(self, nullptr);
            return;
        }

        if (response->type() == Fetch::Infrastructure::Response::Type::Opaque || response->type() == Fetch::Infrastructure::Response::Type::OpaqueRedirect) {
            auto& filtered_response = static_cast<Fetch::Infrastructure::FilteredResponse&>(*response);
            response = filtered_response.internal_response();
        }

        auto on_image_data_read = GC::create_function(self.heap(), [weak_self](ByteBuffer image_data) {
            if (!weak_self)
                return;
            // 6. If an image is thus obtained, the poster frame is that image. Otherwise, there is no poster frame.
            (void)Platform::ImageCodecPlugin::the().decode_image(
                image_data,
                [weak_self](Web::Platform::DecodedImage& image) -> ErrorOr<void> {
                    if (!weak_self)
                        return {};
                    RefPtr<Gfx::Bitmap> poster_frame;
                    if (!image.frames.is_empty())
                        poster_frame = move(image.frames[0].bitmap);
                    finalize(*weak_self, move(poster_frame));
                    return {};
                },
                [weak_self](auto&) {
                    if (!weak_self)
                        return;
                    finalize(*weak_self, nullptr);
                });
        });

        VERIFY(response->body());
        auto on_body_read_error = GC::create_function(self.heap(), [weak_self](JS::Value) {
            if (!weak_self)
                return;
            finalize(*weak_self, nullptr);
        });

        response->body()->fully_read(realm, on_image_data_read, on_body_read_error, GC::Ref { global });
    };

    m_fetch_controller = Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input)));

    return {};
}

HTMLVideoElement::Representation HTMLVideoElement::current_representation() const
{
    // https://html.spec.whatwg.org/multipage/media.html#the-video-element:the-video-element-7
    // A video element represents what is given for the first matching condition in the list below:

    // -> When no video data is available (the element's readyState attribute is either HAVE_NOTHING, or HAVE_METADATA
    //    but no video data has yet been obtained at all, or the element's readyState attribute is any subsequent value
    //    but the media resource does not have a video channel)
    if (ready_state() == ReadyState::HaveNothing || video_tracks()->length() == 0) {
        // The video element represents its poster frame, if any, or else transparent black with no intrinsic dimensions.
        return poster_frame() ? Representation::PosterFrame : Representation::TransparentBlack;
    }

    // -> When the video element is paused, the current playback position is the first frame of video, and the element's
    //    show poster flag is set
    if (paused() && current_playback_position() == 0 && show_poster()) {
        // The video element represents its poster frame, if any, or else the first frame of the video.
        return poster_frame() ? Representation::PosterFrame : Representation::FirstVideoFrame;
    }

    // -> When the video element is paused, and the frame of video corresponding to the current playback position
    //    is not available (e.g. because the video is seeking or buffering)
    //
    //     The video element represents the last frame of the video to have been rendered.
    //
    // NOTE: We don't need to check this condition, as seeking is asynchronous, and the last available frame
    //       will be kept until the seek completes.

    // -> When the video element is neither potentially playing nor paused (e.g. when seeking or stalled)
    if (!potentially_playing() && !paused()) {
        // The video element represents the last frame of the video to have been rendered.
        return Representation::VideoFrame;
    }

    // -> When the video element is paused
    if (paused()) {
        // The video element represents the frame of video corresponding to the current playback position.
        return Representation::VideoFrame;
    }

    // -> Otherwise (the video element has a video channel and is potentially playing)
    //
    //     The video element represents the frame of video at the continuously increasing "current" position. When the
    //     current playback position changes such that the last frame rendered is no longer the frame corresponding to
    //     the current playback position in the video, the new frame must be rendered.
    return Representation::VideoFrame;
}

RefPtr<Gfx::DecodedImageFrame> HTMLVideoElement::current_decoded_image_frame() const
{
    auto const& sink = selected_video_track_sink();
    if (sink == nullptr)
        return nullptr;
    auto current_frame = sink->current_frame();
    if (!current_frame)
        return nullptr;
    auto bitmap_or_error = current_frame->yuv_data().to_bitmap();
    if (bitmap_or_error.is_error()) {
        dbgln("Could not convert video frame to bitmap: {}", bitmap_or_error.release_error());
        return nullptr;
    }
    auto bitmap = bitmap_or_error.release_value();
    return Gfx::DecodedImageFrame::create(NonnullRefPtr<Gfx::Bitmap const> { *bitmap }, current_frame->color_space());
}

}

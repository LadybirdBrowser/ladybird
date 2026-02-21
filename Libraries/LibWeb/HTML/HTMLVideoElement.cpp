/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibWeb/Bindings/HTMLVideoElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
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

// https://html.spec.whatwg.org/multipage/media.html#dom-video-videowidth
u32 HTMLVideoElement::video_width() const
{
    // The videoWidth IDL attribute must return the intrinsic width of the video in CSS pixels. The videoHeight IDL
    // attribute must return the intrinsic height of the video in CSS pixels. If the element's readyState attribute
    // is HAVE_NOTHING, then the attributes must return 0.
    if (ready_state() == ReadyState::HaveNothing)
        return 0;
    return m_video_width;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-video-videoheight
u32 HTMLVideoElement::video_height() const
{
    // The videoWidth IDL attribute must return the intrinsic width of the video in CSS pixels. The videoHeight IDL
    // attribute must return the intrinsic height of the video in CSS pixels. If the element's readyState attribute
    // is HAVE_NOTHING, then the attributes must return 0.
    if (ready_state() == ReadyState::HaveNothing)
        return 0;
    return m_video_height;
}

// https://html.spec.whatwg.org/multipage/media.html#attr-video-poster
WebIDL::ExceptionOr<void> HTMLVideoElement::determine_element_poster_frame(Optional<String> const& poster)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    m_poster_frame = nullptr;

    // 1. If there is an existing instance of this algorithm running for this video element, abort that instance of
    //    this algorithm without changing the poster frame.
    if (m_fetch_controller)
        m_fetch_controller->stop_fetch();

    // 2. If the poster attribute's value is the empty string or if the attribute is absent, then there is no poster
    //    frame; return.
    if (!poster.has_value() || poster->is_empty())
        return {};

    // 3. Let url be the result of encoding-parsing a URL given the poster attribute's value, relative to the element's node document.
    auto url_record = document().encoding_parse_url(*poster);

    // 4. If url is failure, then return.
    if (!url_record.has_value())
        return {};

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
    fetch_algorithms_input.process_response = [this](auto response) mutable {
        ScopeGuard guard { [&] { m_load_event_delayer.clear(); } };

        auto& realm = this->realm();
        auto& global = document().realm().global_object();

        if (response->is_network_error())
            return;

        if (response->type() == Fetch::Infrastructure::Response::Type::Opaque || response->type() == Fetch::Infrastructure::Response::Type::OpaqueRedirect) {
            auto& filtered_response = static_cast<Fetch::Infrastructure::FilteredResponse&>(*response);
            response = filtered_response.internal_response();
        }

        auto on_image_data_read = GC::create_function(heap(), [this](ByteBuffer image_data) mutable {
            m_fetch_controller = nullptr;

            // 6. If an image is thus obtained, the poster frame is that image. Otherwise, there is no poster frame.
            (void)Platform::ImageCodecPlugin::the().decode_image(
                image_data,
                [strong_this = GC::Root(*this)](Web::Platform::DecodedImage& image) -> ErrorOr<void> {
                    if (!image.frames.is_empty())
                        strong_this->m_poster_frame = move(image.frames[0].bitmap);
                    return {};
                },
                [](auto&) {});
        });

        VERIFY(response->body());
        auto empty_algorithm = GC::create_function(heap(), [](JS::Value) { });

        response->body()->fully_read(realm, on_image_data_read, empty_algorithm, GC::Ref { global });
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
    if (ready_state() == HTML::HTMLMediaElement::ReadyState::HaveNothing
        || (ready_state() >= HTML::HTMLMediaElement::ReadyState::HaveMetadata && video_tracks()->length() == 0)) {
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

RefPtr<Gfx::ImmutableBitmap> HTMLVideoElement::bitmap() const
{
    auto const& sink = selected_video_track_sink();
    if (sink == nullptr)
        return nullptr;
    return sink->current_frame();
}

}

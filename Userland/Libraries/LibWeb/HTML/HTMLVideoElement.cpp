/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/HTMLVideoElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/VideoTrack.h>
#include <LibWeb/HTML/VideoTrackList.h>
#include <LibWeb/Layout/VideoBox.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLVideoElement);

HTMLVideoElement::HTMLVideoElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLMediaElement(document, move(qualified_name))
{
}

HTMLVideoElement::~HTMLVideoElement() = default;

void HTMLVideoElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLVideoElement);
}

void HTMLVideoElement::finalize()
{
    Base::finalize();

    for (auto video_track : video_tracks()->video_tracks())
        video_track->stop_video({});
}

void HTMLVideoElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_video_track);
    visitor.visit(m_fetch_controller);
}

void HTMLVideoElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    Base::attribute_changed(name, old_value, value);

    if (name == HTML::AttributeNames::poster) {
        determine_element_poster_frame(value).release_value_but_fixme_should_propagate_errors();
    }
}

JS::GCPtr<Layout::Node> HTMLVideoElement::create_layout_node(NonnullRefPtr<CSS::StyleProperties> style)
{
    return heap().allocate_without_realm<Layout::VideoBox>(document(), *this, move(style));
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

void HTMLVideoElement::set_video_track(JS::GCPtr<HTML::VideoTrack> video_track)
{
    set_needs_style_update(true);
    document().set_needs_layout();

    if (m_video_track)
        m_video_track->pause_video({});

    m_video_track = video_track;
}

void HTMLVideoElement::set_current_frame(Badge<VideoTrack>, RefPtr<Gfx::Bitmap> frame, double position)
{
    m_current_frame = { move(frame), position };
    if (paintable())
        paintable()->set_needs_display();
}

void HTMLVideoElement::on_playing()
{
    if (m_video_track)
        m_video_track->play_video({});
}

void HTMLVideoElement::on_paused()
{
    if (m_video_track)
        m_video_track->pause_video({});
}

void HTMLVideoElement::on_seek(double position, MediaSeekMode seek_mode)
{
    if (m_video_track)
        m_video_track->seek(AK::Duration::from_milliseconds(position * 1000.0), seek_mode);
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

    // 3. Parse the poster attribute's value relative to the element's node document. If this fails, then there is no
    //    poster frame; return.
    auto url_record = document().parse_url(*poster);
    if (!url_record.is_valid())
        return {};

    // 4. Let request be a new request whose URL is the resulting URL record, client is the element's node document's
    //    relevant settings object, destination is "image", initiator type is "video", credentials mode is "include",
    //    and whose use-URL-credentials flag is set.
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(move(url_record));
    request->set_client(&document().relevant_settings_object());
    request->set_destination(Fetch::Infrastructure::Request::Destination::Image);
    request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Video);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);
    request->set_use_url_credentials(true);

    // 5. Fetch request. This must delay the load event of the element's node document.
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    m_load_event_delayer.emplace(document());

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

        auto on_image_data_read = JS::create_heap_function(heap(), [this](ByteBuffer image_data) mutable {
            m_fetch_controller = nullptr;

            // 6. If an image is thus obtained, the poster frame is that image. Otherwise, there is no poster frame.
            (void)Platform::ImageCodecPlugin::the().decode_image(
                image_data,
                [strong_this = JS::Handle(*this)](Web::Platform::DecodedImage& image) -> ErrorOr<void> {
                    if (!image.frames.is_empty())
                        strong_this->m_poster_frame = move(image.frames[0].bitmap);
                    return {};
                },
                [](auto&) {});
        });

        VERIFY(response->body());
        auto empty_algorithm = JS::create_heap_function(heap(), [](JS::Value) {});

        response->body()->fully_read(realm, on_image_data_read, empty_algorithm, JS::NonnullGCPtr { global });
    };

    m_fetch_controller = TRY(Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));

    return {};
}

}

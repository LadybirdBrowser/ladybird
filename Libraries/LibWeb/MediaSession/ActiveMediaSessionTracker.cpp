/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/RootVector.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaSession/ActiveMediaSessionTracker.h>
#include <LibWeb/MediaSession/MediaMetadata.h>
#include <LibWeb/MediaSession/MediaSession.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/Promise.h>
#include <math.h>

namespace Web::MediaSession {

ActiveMediaSessionTracker::ActiveMediaSessionTracker(Page& page)
    : m_page(page)
    , m_weak_page(page)
{
}

static bool is_audible_and_controllable(HTML::HTMLMediaElement const& element)
{
    return element.is_playing_audio() && element.duration() >= 5
        && !element.src_object().has<GC::Ref<MediaCapture::MediaStream>>();
}

// https://w3c.github.io/mediasession/#active-media-session
void ActiveMediaSessionTracker::media_element_became_audible(HTML::HTMLMediaElement& element)
{
    auto& document = element.document();
    if (!document.is_fully_active() || document.has_been_destroyed() || !is_audible_and_controllable(element))
        return;

    auto controlled = m_controlled_elements.find_if([&](auto const& entry) { return entry.element_id == element.unique_id(); });
    if (!controlled.is_end()) {
        if (controlled->audible_and_controllable)
            return;
        controlled->activation_order = ++m_activation_counter;
        controlled->audible_and_controllable = true;
    } else {
        m_controlled_elements.append({ element.unique_id(), document.unique_id(), ++m_activation_counter, true });
    }

    auto session = document.window()->navigator()->media_session();
    // A session counts as paused until its media first plays, so a position the page set before that starts
    // advancing only once playback does.
    m_position_states.ensure(document.unique_id(), [&] {
        return PositionState { session->position_state(), session->position_state(), Bindings::MediaSessionPlaybackState::Paused };
    });
    mark_dirty();
}

void ActiveMediaSessionTracker::media_element_removed(HTML::HTMLMediaElement& element)
{
    m_controlled_elements.remove_first_matching([&](auto const& entry) { return entry.element_id == element.unique_id(); });
    mark_dirty();
}

void ActiveMediaSessionTracker::media_element_unregistered(UniqueNodeID element_id)
{
    if (!m_controlled_elements.remove_first_matching([&](auto const& entry) { return entry.element_id == element_id; }))
        return;

    // Unregistration runs during finalization, so defer GC object access and allocation until collection ends.
    GC::Heap::the().enqueue_post_gc_task([page = m_weak_page] {
        if (page)
            page->active_media_session_tracker().mark_dirty();
    });
}

void ActiveMediaSessionTracker::document_became_inactive(DOM::Document& document)
{
    m_controlled_elements.remove_all_matching([&](auto const& entry) { return entry.document_id == document.unique_id(); });
    mark_dirty();
}

void ActiveMediaSessionTracker::update_active_document()
{
    m_controlled_elements.remove_all_matching([](ControlledElement const& entry) {
        auto* element = as_if<HTML::HTMLMediaElement>(DOM::Node::from_unique_id(entry.element_id));
        return !element || element->document().unique_id() != entry.document_id
            || !element->document().is_fully_active() || element->document().has_been_destroyed()
            || element->src_object().has<GC::Ref<MediaCapture::MediaStream>>();
    });

    auto previous_document = m_active_document;
    m_active_document.clear();
    u64 latest_activation = 0;
    for (auto& entry : m_controlled_elements) {
        auto& element = as<HTML::HTMLMediaElement>(*DOM::Node::from_unique_id(entry.element_id));
        if (!is_audible_and_controllable(element))
            entry.audible_and_controllable = false;
        if (entry.activation_order > latest_activation) {
            latest_activation = entry.activation_order;
            m_active_document = entry.document_id;
        }
    }
    if (m_active_document != previous_document)
        m_paused_by_user_agent.clear();
    else
        m_paused_by_user_agent.remove_all_matching([&](auto id) {
            return !m_controlled_elements.contains([&](auto const& entry) { return entry.element_id == id; });
        });
    m_position_states.remove_all_matching([](auto id, auto const&) {
        auto* document = as_if<DOM::Document>(DOM::Node::from_unique_id(id));
        return !document || !document->is_fully_active() || document->has_been_destroyed();
    });
}

Bindings::MediaSessionPlaybackState ActiveMediaSessionTracker::actual_playback_state(DOM::Document& document)
{
    auto session = document.window()->navigator()->media_session();

    // https://w3c.github.io/mediasession/#actual-playback-state
    // If the declared playback state is playing, return playing.
    // Otherwise, return the guessed playback state.
    auto playback_state = session->playback_state() == Bindings::MediaSessionPlaybackState::Playing
        ? Bindings::MediaSessionPlaybackState::Playing
        : Bindings::MediaSessionPlaybackState::Paused;

    // https://w3c.github.io/mediasession/#guessed-playback-state
    // The browsing context's guessed playback state is "playing" if any of them is potentially playing and not muted,
    // and is "paused" otherwise.
    m_page->for_each_media_element([&](auto& element) {
        if (&element.document() == &document && element.potentially_playing() && !element.muted())
            playback_state = Bindings::MediaSessionPlaybackState::Playing;
    });
    return playback_state;
}

void ActiveMediaSessionTracker::update_position_states()
{
    for (auto& entry : m_position_states) {
        auto& document = as<DOM::Document>(*DOM::Node::from_unique_id(entry.key));
        auto session = document.window()->navigator()->media_session();
        auto playback_state = actual_playback_state(document);
        auto& position = entry.value;
        if (position.explicit_position != session->position_state()) {
            position.explicit_position = session->position_state();
            position.reported_position = position.explicit_position;
        } else if (position.reported_position.has_value() && position.playback_state != playback_state) {
            auto& reported = *position.reported_position;
            auto now = MonotonicTime::now();
            if (position.playback_state == Bindings::MediaSessionPlaybackState::Playing) {
                auto elapsed = (now - reported.last_position_updated_time).to_seconds_f64();
                reported.position = clamp(reported.position + elapsed * reported.playback_rate, 0.0, reported.duration);
            }
            reported.last_position_updated_time = now;
        }
        position.playback_state = playback_state;
    }
}

GC::Ptr<DOM::Document> ActiveMediaSessionTracker::active_document()
{
    // A document stops being fully active without telling the tracker when its iframe leaves the tree.
    update_active_document();
    if (!m_active_document.has_value())
        return nullptr;
    auto* document = as_if<DOM::Document>(DOM::Node::from_unique_id(*m_active_document));
    if (!document || !document->is_fully_active() || document->has_been_destroyed())
        return nullptr;
    return document;
}

// https://w3c.github.io/mediasession/#handle-media-session-action
void ActiveMediaSessionTracker::handle_action(Bindings::MediaSessionAction action, Optional<double> seek_offset, Optional<double> seek_time, Optional<bool> fast_seek)
{
    // 1. Let session be source's target.
    // 2. If session is null, set session to the active media session.
    // NB: Actions from the platform don't have an explicit target.
    auto document = active_document();

    // 3. If session is null, abort these steps.
    if (!document)
        return;
    auto session = document->window()->navigator()->media_session();

    // 4. Let actions be session's supported media session actions.
    auto const& actions = session->action_handlers();

    // 5. If actions does not contain the key action, abort these steps.
    auto entry = actions.find(action);
    if (entry == actions.end()) {
        // AD-HOC: The spec aborts without a handler. OS media keys and Now Playing controls must also work on pages
        //         that never call setActionHandler, so apply the default action to the controlled elements.
        run_default_action(action, seek_offset, seek_time, fast_seek);
        return;
    }

    // 6. Let handler be the MediaSessionActionHandler associated with the key action in actions.
    auto handler = entry->value;
    auto& realm = handler->associated_realm();
    HTML::TemporaryExecutionContext execution_context { realm };
    auto details = JS::Object::create(realm, realm.intrinsics().object_prototype());
    details->define_direct_property("action"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(action)), JS::default_attributes);
    if (seek_offset.has_value())
        details->define_direct_property("seekOffset"_utf16_fly_string, JS::Value(*seek_offset), JS::default_attributes);
    if (seek_time.has_value())
        details->define_direct_property("seekTime"_utf16_fly_string, JS::Value(*seek_time), JS::default_attributes);
    if (fast_seek.has_value())
        details->define_direct_property("fastSeek"_utf16_fly_string, JS::Value(*fast_seek), JS::default_attributes);

    // 8. Run the activation notification steps in the browsing context associated with session.
    // AD-HOC: The spec grants activation after the callback. Grant it before the callback so the handler can use
    //         APIs that require user activation while responding to the user's media control.
    HTML::Window::run_activation_notification_steps(*document);

    // 7. Run handler with the details parameter set to: MediaSessionActionDetails.
    (void)WebIDL::invoke_callback(*handler, {}, WebIDL::ExceptionBehavior::Report, { { details } });
}

void ActiveMediaSessionTracker::run_default_action(Bindings::MediaSessionAction action, Optional<double> seek_offset, Optional<double> seek_time, Optional<bool> fast_seek)
{
    switch (action) {
    case Bindings::MediaSessionAction::Play:
    case Bindings::MediaSessionAction::Pause:
    case Bindings::MediaSessionAction::Stop:
    case Bindings::MediaSessionAction::Seekbackward:
    case Bindings::MediaSessionAction::Seekforward:
        break;
    case Bindings::MediaSessionAction::Seekto:
        if (!seek_time.has_value())
            return;
        break;
    default:
        return;
    }

    auto document = active_document();
    if (!document)
        return;
    HTML::TemporaryExecutionContext execution_context { document->relevant_settings_object() };
    HTML::Window::run_activation_notification_steps(*document);

    Vector<UniqueNodeID> elements;
    Optional<UniqueNodeID> most_recent_element;
    u64 latest_activation = 0;
    for (auto const& entry : m_controlled_elements) {
        if (entry.document_id != document->unique_id())
            continue;
        auto* element = as_if<HTML::HTMLMediaElement>(DOM::Node::from_unique_id(entry.element_id));
        if (!element || &element->document() != document.ptr())
            continue;
        elements.append(entry.element_id);
        if (entry.activation_order > latest_activation) {
            latest_activation = entry.activation_order;
            most_recent_element = entry.element_id;
        }
    }

    if (action == Bindings::MediaSessionAction::Play) {
        auto paused_elements = move(m_paused_by_user_agent);
        m_paused_by_user_agent.clear();
        paused_elements.remove_all_matching([&](auto id) { return !elements.contains_slow(id); });
        if (paused_elements.is_empty() && most_recent_element.has_value())
            paused_elements.append(*most_recent_element);
        for (auto id : paused_elements) {
            auto* element = as_if<HTML::HTMLMediaElement>(DOM::Node::from_unique_id(id));
            if (!element)
                continue;
            auto promise = WebIDL::create_promise_for(*element);
            WebIDL::mark_promise_as_handled(promise);
            element->play(promise);
        }
        return;
    }

    if (action == Bindings::MediaSessionAction::Pause || action == Bindings::MediaSessionAction::Stop) {
        Vector<UniqueNodeID> paused_elements;
        for (auto id : elements) {
            auto* element = as_if<HTML::HTMLMediaElement>(DOM::Node::from_unique_id(id));
            if (!element || element->paused())
                continue;
            paused_elements.append(id);
            element->pause();
        }
        if (action == Bindings::MediaSessionAction::Stop) {
            m_paused_by_user_agent.clear();
            m_controlled_elements.remove_all_matching([&](auto const& entry) {
                return entry.document_id == document->unique_id();
            });
            mark_dirty();
        } else if (!paused_elements.is_empty()) {
            m_paused_by_user_agent = move(paused_elements);
        }
        return;
    }

    for (auto id : elements) {
        auto* element = as_if<HTML::HTMLMediaElement>(DOM::Node::from_unique_id(id));
        if (!element || !isfinite(element->duration()))
            continue;
        double position;
        if (action == Bindings::MediaSessionAction::Seekto) {
            position = min(*seek_time, element->duration());
        } else {
            auto offset = seek_offset.value_or(15.0);
            if (action == Bindings::MediaSessionAction::Seekbackward)
                offset = -offset;
            position = clamp(element->current_time() + offset, 0.0, element->duration());
        }
        if (action == Bindings::MediaSessionAction::Seekto && fast_seek.value_or(false))
            element->fast_seek(position);
        else
            element->set_current_time(position);
    }
}

void ActiveMediaSessionTracker::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_page);
    visitor.visit(m_artwork_metadata);
    visitor.visit(m_artwork_fetch_controller);
    visitor.visit(m_artwork_waiters);
}

static Vector<URL::URL> ranked_artwork_urls(Vector<Bindings::MediaImage> const& images)
{
    struct Candidate {
        size_t index;
        unsigned category { 2 };
        u32 distance { NumericLimits<u32>::max() };
        bool square { false };
    };
    Vector<Candidate> candidates;
    for (size_t index = 0; index < images.size(); ++index) {
        Candidate candidate { index };
        auto sizes = MUST(images[index].sizes.utf16_view().to_utf8());
        for (auto token : sizes.bytes_as_string_view().split_view_if([](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f'; })) {
            if (token.equals_ignoring_ascii_case("any"sv)) {
                candidate.category = 0;
                break;
            }
            auto separator = token.find_any_of("xX"sv);
            if (!separator.has_value())
                continue;
            auto width_text = token.substring_view(0, *separator);
            auto height_text = token.substring_view(*separator + 1);
            if (width_text.is_empty() || height_text.is_empty()
                || !all_of(width_text, [](char ch) { return is_ascii_digit(ch); })
                || !all_of(height_text, [](char ch) { return is_ascii_digit(ch); }))
                continue;
            auto width = width_text.to_number<u32>();
            auto height = height_text.to_number<u32>();
            if (!width.has_value() || !height.has_value() || *width == 0 || *height == 0)
                continue;
            auto deviation = [](u32 side) { return side > artwork_maximum_side ? side - artwork_maximum_side : artwork_maximum_side - side; };
            auto distance = max(deviation(*width), deviation(*height));
            bool square = width == height;
            if (distance < candidate.distance || (distance == candidate.distance && square)) {
                candidate.distance = distance;
                candidate.square = square;
            }
            candidate.category = 1;
        }
        candidates.append(candidate);
    }
    quick_sort(candidates, [](auto const& a, auto const& b) {
        if (a.category != b.category)
            return a.category < b.category;
        if (a.category == 1) {
            if (a.distance != b.distance)
                return a.distance < b.distance;
            if (a.square != b.square)
                return a.square;
        }
        return a.index < b.index;
    });
    Vector<URL::URL> urls;
    for (auto const& candidate : candidates)
        urls.append(DOMURL::parse(images[candidate.index].src).release_value());
    return urls;
}

// https://w3c.github.io/mediasession/#fetch-image-algorithm
void ActiveMediaSessionTracker::update_artwork()
{
    auto document = active_document();
    Optional<UniqueNodeID> document_id;
    GC::Ptr<MediaMetadata> metadata;
    if (document) {
        document_id = document->unique_id();
        metadata = document->window()->navigator()->media_session()->metadata();
    }
    auto version = metadata ? metadata->artwork_version() : 0;
    if (document_id == m_artwork_document && metadata == m_artwork_metadata && version == m_artwork_version)
        return;

    auto urls = metadata ? ranked_artwork_urls(metadata->artwork_images()) : Vector<URL::URL> {};
    bool same_document = document_id == m_artwork_document;
    m_artwork_document = document_id;
    m_artwork_metadata = metadata;
    m_artwork_version = version;

    // Text changes don't restart a fetch. A decoded image remains usable while the preferred URL is unchanged.
    bool same_preferred_url = !urls.is_empty() && !m_artwork_urls.is_empty() && urls.first() == m_artwork_urls.first();
    bool can_reuse_artwork = m_artwork && m_next_artwork_index == 1 && same_preferred_url;
    bool previous_fetch_failed = !m_artwork && !m_artwork_fetch_in_flight;
    if (same_document && !previous_fetch_failed && (urls == m_artwork_urls || can_reuse_artwork)) {
        m_artwork_urls = move(urls);
        return;
    }

    // 1. If there are other fetch image algorithms running, cancel existing algorithm execution instances.
    ++m_artwork_generation;
    if (auto controller = exchange(m_artwork_fetch_controller, nullptr))
        controller->stop_fetch();
    m_artwork = nullptr;
    m_artwork_urls.clear();
    m_next_artwork_index = 0;

    // 2. If metadata's artwork of the active media session is empty, then terminate these steps.
    if (urls.is_empty()) {
        finish_artwork(nullptr);
        return;
    }

    // 3. If the platform supports displaying media artwork, select a preferred artwork image from metadata's artwork
    //    of the active media session.
    // NB: Rank by the larger of the width and height deviations from 512, preferring squares on ties. Array order
    //     breaks equal ranks. Entries with "any" precede declared sizes, which precede entries without usable sizes.
    m_artwork_urls = move(urls);
    m_artwork_fetch_in_flight = true;
    fetch_next_artwork();
}

static ErrorOr<NonnullRefPtr<Gfx::Bitmap const>> scale_artwork(Page& page, Gfx::Bitmap const& bitmap)
{
    auto longest_side = max(bitmap.width(), bitmap.height());
    if (longest_side <= artwork_maximum_side)
        return bitmap;
    if (!page.has_compositor_host())
        return Error::from_string_literal("No compositor to scale media session artwork");

    auto scale = static_cast<double>(artwork_maximum_side) / longest_side;
    Gfx::IntSize size { max(1, static_cast<int>(round(bitmap.width() * scale))), max(1, static_cast<int>(round(bitmap.height() * scale))) };
    auto output = TRY(Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size));
    auto frame = TRY(Gfx::DecodedImageFrame::create(bitmap));
    if (!Painting::rasterize_image_frame(page.compositor_host(), frame, output->rect().to_type<float>(), Gfx::ScalingMode::BilinearMipmap, output))
        return Error::from_string_literal("Unable to scale media session artwork");
    return output;
}

bool ActiveMediaSessionTracker::artwork_document_is_active()
{
    auto document = active_document();
    return document && document->unique_id() == m_artwork_document;
}

// https://w3c.github.io/mediasession/#fetch-image-algorithm
void ActiveMediaSessionTracker::fetch_next_artwork()
{
    auto document = active_document();
    if (!document || document->unique_id() != m_artwork_document) {
        abandon_artwork_fetch();
        return;
    }
    if (m_next_artwork_index == m_artwork_urls.size()) {
        finish_artwork(nullptr);
        return;
    }
    auto& settings = document->relevant_settings_object();

    // 4. Let request be a new request whose URL is the preferred artwork image's src, destination is "image", mode is
    //    "no-cors", credentials mode is "include", and whose use-URL-credentials flag is set.
    auto request = Fetch::Infrastructure::Request::create();
    request->set_url(m_artwork_urls[m_next_artwork_index++]);
    request->set_destination(Fetch::Infrastructure::Request::Destination::Image);
    request->set_mode(Fetch::Infrastructure::Request::Mode::NoCORS);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);
    request->set_use_url_credentials(true);
    request->set_client(settings.address());

    // 5. Fetch request with the following steps executed as the fetch's processResponse algorithm.
    // NB: Consume the body before handing its bytes to the image decoder.
    Fetch::Infrastructure::FetchAlgorithms::Input algorithms { Fetch::Engine::BodyIntent::ConsumeForCallback };
    algorithms.process_response_consume_body = GC::create_function(GC::Heap::the(), GC::captures(*m_page, m_artwork_generation), [](Page& page, u64 generation, GC::Ref<Fetch::Infrastructure::ResponseView> response, Fetch::Infrastructure::FetchAlgorithms::BodyBytes bytes) {
        auto& tracker = page.active_media_session_tracker();
        if (generation != tracker.m_artwork_generation)
            return;
        if (!tracker.artwork_document_is_active()) {
            tracker.abandon_artwork_fetch();
            return;
        }
        tracker.m_artwork_fetch_controller = nullptr;

        // 1. Let response be the response given to the fetch's processResponse algorithm.
        // 2. If response's type is not "error", attempt to decode response's body as an image.
        if (response->is_network_error() || !bytes.has<Core::ImmutableBytes>()) {
            tracker.fetch_next_artwork();
            return;
        }
        auto on_decoded = [page = GC::Root(page), generation](Platform::DecodedImage& image) -> ErrorOr<void> {
            // Platform artwork uses only the first frame, so release any animation session, even for a stale decode.
            if (image.session_id != 0)
                Platform::ImageCodecPlugin::the().stop_animation_decode(image.session_id);
            auto& tracker = page->active_media_session_tracker();
            if (generation != tracker.m_artwork_generation)
                return {};
            if (!tracker.artwork_document_is_active()) {
                tracker.abandon_artwork_fetch();
                return {};
            }

            // 3. If the image format is supported, use the image as the artwork for display in platform UI. Otherwise
            //    the fetch image algorithm fails and terminates.
            // AD-HOC: If an entry fails, try the next ranked entry before giving up on the session's artwork.
            if (!image.frames.is_empty() && image.frames.first().bitmap && !image.frames.first().bitmap->size().is_empty()) {
                auto bitmap = scale_artwork(*page, *image.frames.first().bitmap);
                if (generation != tracker.m_artwork_generation)
                    return {};
                if (!tracker.artwork_document_is_active()) {
                    tracker.abandon_artwork_fetch();
                    return {};
                }
                if (!bitmap.is_error()) {
                    tracker.finish_artwork(bitmap.release_value());
                    return {};
                }
            }
            tracker.fetch_next_artwork();
            return {};
        };
        auto on_failed = [page = GC::Root(page), generation](Error&) {
            auto& tracker = page->active_media_session_tracker();
            if (generation == tracker.m_artwork_generation)
                tracker.fetch_next_artwork();
        };
        (void)Platform::ImageCodecPlugin::the().decode_image(bytes.get<Core::ImmutableBytes>().bytes(), move(on_decoded), move(on_failed));
    });
    m_artwork_fetch_controller = Fetch::Fetching::fetch(settings.realm(), request, Fetch::Infrastructure::FetchAlgorithms::create(move(algorithms)));
}

// The document the artwork list belongs to stopped being the active one mid-fetch. Forget it, so the next update builds
// the list again for whichever document is active, and fetch again then.
void ActiveMediaSessionTracker::abandon_artwork_fetch()
{
    m_artwork_document.clear();
    finish_artwork(nullptr);
}

void ActiveMediaSessionTracker::finish_artwork(RefPtr<Gfx::Bitmap const> bitmap)
{
    m_artwork_fetch_controller = nullptr;
    m_artwork_fetch_in_flight = false;
    m_artwork = move(bitmap);
    mark_dirty();
}

void ActiveMediaSessionTracker::when_artwork_settled(GC::Ref<WebIDL::Promise> promise)
{
    update_artwork();
    if (!m_artwork_fetch_in_flight) {
        WebIDL::resolve_promise(promise);
        return;
    }
    m_artwork_waiters.append(promise);
}

void ActiveMediaSessionTracker::mark_dirty()
{
    update_active_document();
    // Re-anchor before coalescing reports so a pause and resume in one task can't advance through the paused interval.
    update_position_states();
    if (m_report_scheduled)
        return;
    m_report_scheduled = true;
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*m_page), [](Page& page) {
        page.active_media_session_tracker().report_state();
    }));
}

Optional<MediaSessionState> ActiveMediaSessionTracker::compute_state()
{
    update_artwork();
    update_position_states();

    // https://w3c.github.io/mediasession/#update-metadata-algorithm
    // 1. If the active media session is null, unset the media metadata presented to the platform,
    //    and terminate these steps.
    auto document = active_document();
    if (!document)
        return {};

    auto session = document->window()->navigator()->media_session();
    MediaSessionState state;
    state.activation_counter = m_activation_counter;

    // https://w3c.github.io/mediasession/#update-metadata-algorithm
    // 2. If the metadata of the active media session is an empty metadata, unset the media metadata presented to the
    //    platform, and terminate these steps.
    // 3. Update the media metadata presented to the platform to match the metadata for the active media session.
    // NB: Empty strings and a null bitmap represent unset metadata.
    if (auto metadata = session->metadata()) {
        state.title = MUST(metadata->title().utf16_view().to_utf8());
        state.artist = MUST(metadata->artist().utf16_view().to_utf8());
        state.album = MUST(metadata->album().utf16_view().to_utf8());
        state.artwork = m_artwork;
    }

    // 4. If the user agent wants to display an artwork image, the user agent MUST run the fetch image algorithm.
    // NB: update_artwork() starts the fetch without delaying the text metadata report.

    auto const& position_state = m_position_states.get(*m_active_document).value();
    state.playback_state = position_state.playback_state;

    // https://w3c.github.io/mediasession/#position-state
    state.position = position_state.reported_position;
    size_t controlled_element_count = 0;
    for (auto const& entry : m_controlled_elements) {
        if (entry.document_id == *m_active_document)
            ++controlled_element_count;
    }
    bool has_finite_duration = false;
    for (auto const& entry : m_controlled_elements) {
        if (entry.document_id != *m_active_document)
            continue;
        auto& element = as<HTML::HTMLMediaElement>(*DOM::Node::from_unique_id(entry.element_id));
        if (!isfinite(element.duration()))
            continue;
        has_finite_duration = true;
        if (!state.position.has_value() && controlled_element_count == 1) {
            state.position = MediaSessionPositionState {
                element.duration(), element.potentially_playing() ? element.playback_rate() : 0.0,
                element.current_time(), MonotonicTime::now()
            };
        }
    }

    // https://w3c.github.io/mediasession/#media-session-actions-update-algorithm
    // 1. Let available actions be an array of media session actions.
    // 3. Otherwise, set the available actions to the list of keys available in the active media session's supported
    //    media session actions.
    for (auto const& entry : session->action_handlers())
        state.supported_actions.append(entry.key);

    // AD-HOC: The spec lists only registered handlers here. Include actions the user agent can perform on controlled
    //         elements too, so removing a handler restores the corresponding default control.
    auto add_default_action = [&](auto action) {
        if (!state.supported_actions.contains_slow(action))
            state.supported_actions.append(action);
    };
    add_default_action(Bindings::MediaSessionAction::Play);
    add_default_action(Bindings::MediaSessionAction::Pause);
    add_default_action(Bindings::MediaSessionAction::Stop);
    if (has_finite_duration) {
        add_default_action(Bindings::MediaSessionAction::Seekto);
        add_default_action(Bindings::MediaSessionAction::Seekbackward);
        add_default_action(Bindings::MediaSessionAction::Seekforward);
    }
    quick_sort(state.supported_actions);
    return state;
}

static bool states_match(Optional<MediaSessionState> const& previous, Optional<MediaSessionState> const& current)
{
    if (previous == current)
        return true;
    if (!previous.has_value() || !current.has_value())
        return false;
    if (previous->activation_counter != current->activation_counter
        || previous->title != current->title || previous->artist != current->artist || previous->album != current->album
        || previous->artwork != current->artwork
        || previous->playback_state != current->playback_state || previous->supported_actions != current->supported_actions)
        return false;
    if (!previous->position.has_value() || !current->position.has_value())
        return false;

    auto const& old_position = *previous->position;
    auto const& new_position = *current->position;
    if (old_position.duration != new_position.duration || old_position.playback_rate != new_position.playback_rate)
        return false;

    auto expected_position = old_position.position;
    if (previous->playback_state == Bindings::MediaSessionPlaybackState::Playing) {
        auto elapsed = (new_position.last_position_updated_time - old_position.last_position_updated_time).to_seconds_f64();
        expected_position += elapsed * old_position.playback_rate;
    }
    expected_position = clamp(expected_position, 0.0, old_position.duration);
    return fabs(new_position.position - expected_position) <= 0.1;
}

void ActiveMediaSessionTracker::report_state()
{
    m_report_scheduled = false;
    auto state = compute_state();
    if (!states_match(m_last_reported_state, state)) {
        m_last_reported_state = state;
        m_page->client().page_did_change_media_session_state(state);
    }

    // Settle waiters after the coalesced update, since metadata can change while a decode is pending.
    if (!m_artwork_fetch_in_flight) {
        GC::RootVector<GC::Ref<WebIDL::Promise>> waiters { move(m_artwork_waiters) };
        m_artwork_waiters.clear();
        for (auto const& promise : waiters) {
            HTML::TemporaryExecutionContext context { WebIDL::promise_realm(promise) };
            WebIDL::resolve_promise(promise);
        }
    }
}

}

/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTrackElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLTrackElement.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTrackElement);

HTMLTrackElement::HTMLTrackElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
    m_track = TextTrack::create(document.realm());
}

HTMLTrackElement::~HTMLTrackElement() = default;

void HTMLTrackElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTrackElement);
}

void HTMLTrackElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_track);
    visitor.visit(m_fetch_algorithms);
    visitor.visit(m_fetch_controller);
}

void HTMLTrackElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // https://html.spec.whatwg.org/multipage/media.html#sourcing-out-of-band-text-tracks
    // As the kind, label, and srclang attributes are set, changed, or removed, the text track must update accordingly, as per the definitions above.
    if (name.equals_ignoring_ascii_case(HTML::AttributeNames::kind)) {
        m_track->set_kind(text_track_kind_from_string(value.value_or({})));
    } else if (name.equals_ignoring_ascii_case(HTML::AttributeNames::label)) {
        m_track->set_label(value.value_or({}));
    } else if (name.equals_ignoring_ascii_case(HTML::AttributeNames::srclang)) {
        m_track->set_language(value.value_or({}));
    } else if (name.equals_ignoring_ascii_case(HTML::AttributeNames::src)) {
        // https://html.spec.whatwg.org/multipage/media.html#sourcing-out-of-band-text-tracks:attr-track-src
        // FIXME: Whenever a track element has its src attribute set, changed, or removed, the user agent must immediately empty the element's text track's text track list of cues.
        //        (This also causes the algorithm above to stop adding cues from the resource being obtained using the previously given URL, if any.)

        if (!value.has_value())
            return;

        // https://html.spec.whatwg.org/multipage/media.html#attr-track-src
        // When the element's src attribute is set, run these steps:
        // 1. Let trackURL be failure.
        Optional<String> track_url;

        // 2. Let value be the element's src attribute value.
        // 3. If value is not the empty string, then set trackURL to the result of encoding-parsing-and-serializing a URL given value, relative to the element's node document.
        if (!value->is_empty())
            track_url = document().encoding_parse_and_serialize_url(value.value_or({}));

        // 4. Set the element's track URL to trackURL if it is not failure; otherwise to the empty string.
        set_track_url(track_url.value_or({}));
    }
    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-id
    // For tracks that correspond to track elements, the track's identifier is the value of the element's id attribute, if any.
    if (name.equals_ignoring_ascii_case(HTML::AttributeNames::id)) {
        m_track->set_id(value.value_or({}));
    }
}

void HTMLTrackElement::inserted()
{
    HTMLElement::inserted();

    // AD-HOC: This is a hack to allow tracks to start loading, without needing to implement the entire
    //         "honor user preferences for automatic text track selection" AO detailed here:
    //         https://html.spec.whatwg.org/multipage/media.html#honor-user-preferences-for-automatic-text-track-selection
    m_track->set_mode(Bindings::TextTrackMode::Hidden);

    start_the_track_processing_model();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-track-readystate
WebIDL::UnsignedShort HTMLTrackElement::ready_state()
{
    // The readyState attribute must return the numeric value corresponding to the text track readiness state of the track element's text track, as defined by the following list:
    switch (m_track->readiness_state()) {
    case TextTrack::ReadinessState::NotLoaded:
        // NONE (numeric value 0)
        //    The text track not loaded state.
        return 0;
    case TextTrack::ReadinessState::Loading:
        // LOADING (numeric value 1)
        //    The text track loading state.
        return 1;
    case TextTrack::ReadinessState::Loaded:
        // LOADED (numeric value 2)
        //    The text track loaded state.
        return 2;
    case TextTrack::ReadinessState::FailedToLoad:
        // ERROR (numeric value 3)
        //    The text track failed to load state.
        return 3;
    }

    VERIFY_NOT_REACHED();
}

void HTMLTrackElement::set_track_url(String track_url)
{
    if (m_track_url == track_url)
        return;

    m_track_url = move(track_url);

    // https://html.spec.whatwg.org/multipage/media.html#start-the-track-processing-model
    if (m_loading && m_fetch_controller && first_is_one_of(m_track->mode(), Bindings::TextTrackMode::Hidden, Bindings::TextTrackMode::Showing)) {
        m_loading = false;
        m_fetch_controller->abort(realm(), {});
    }
}

// https://html.spec.whatwg.org/multipage/media.html#start-the-track-processing-model
void HTMLTrackElement::start_the_track_processing_model()
{
    // 1. If another occurrence of this algorithm is already running for this text track and its track element, return,
    //    letting that other algorithm take care of this element.
    if (m_loading)
        return;

    // 2. If the text track's text track mode is not set to one of hidden or showing, then return.
    if (!first_is_one_of(m_track->mode(), Bindings::TextTrackMode::Hidden, Bindings::TextTrackMode::Showing))
        return;

    // 3. If the text track's track element does not have a media element as a parent, return.
    if (!is<HTMLMediaElement>(parent_element()))
        return;

    // 4. Run the remainder of these steps in parallel, allowing whatever caused these steps to run to continue.
    auto& realm = this->realm();
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm] {
        m_loading = true;
        start_the_track_processing_model_parallel_steps(realm);
    }));
}

void HTMLTrackElement::start_the_track_processing_model_parallel_steps(JS::Realm& realm)
{
    // 5. Top: Await a stable state. The synchronous section consists of the following steps.

    // 6. ⌛ Set the text track readiness state to loading.
    m_track->set_readiness_state(TextTrack::ReadinessState::Loading);

    // 7. ⌛ Let URL be the track URL of the track element.
    auto url = track_url();

    // 8. ⌛ If the track element's parent is a media element then let corsAttributeState be the state of the
    //    parent media element's crossorigin content attribute. Otherwise, let corsAttributeState be No CORS.
    auto cors_attribute_state = CORSSettingAttribute::NoCORS;
    if (is<HTMLMediaElement>(parent())) {
        cors_attribute_state = as<HTMLMediaElement>(parent())->crossorigin();
    }

    // 9. End the synchronous section, continuing the remaining steps in parallel.

    auto fire_error_event = [&]() {
        queue_an_element_task(Task::Source::DOMManipulation, [this, &realm]() {
            m_track->set_readiness_state(TextTrack::ReadinessState::FailedToLoad);
            dispatch_event(DOM::Event::create(realm, HTML::EventNames::error));
        });
    };

    // 10. If URL is not the empty string, then:
    if (!url.is_empty()) {
        // 1. Let request be the result of creating a potential-CORS request given URL, "track", and corsAttributeState,
        // and with the same-origin fallback flag set.
        auto request = create_potential_CORS_request(realm.vm(), url, Fetch::Infrastructure::Request::Destination::Track, cors_attribute_state, SameOriginFallbackFlag::Yes);

        // 2. Set request's client to the track element's node document's relevant settings object.
        request->set_client(&document().relevant_settings_object());

        // 3. Set request's initiator type to "track".
        request->set_initiator_type(Fetch::Infrastructure::Request::InitiatorType::Track);

        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
        fetch_algorithms_input.process_response_consume_body = [this, &realm, &fire_error_event](auto response, auto body_bytes) {
            m_loading = false;

            // If fetching fails for any reason (network error, the server returns an error code, CORS fails, etc.),
            // or if URL is the empty string, then queue an element task on the DOM manipulation task source given the media element
            // to first change the text track readiness state to failed to load and then fire an event named error at the track element.
            if (!response->url().has_value() || body_bytes.template has<Empty>() || body_bytes.template has<Fetch::Infrastructure::FetchAlgorithms::ConsumeBodyFailureTag>() || !Fetch::Infrastructure::is_ok_status(response->status()) || response->is_network_error()) {
                fire_error_event();
                return;
            }

            // If fetching does not fail, but the type of the resource is not a supported text track format, or the file was not successfully processed
            // (e.g., the format in question is an XML format and the file contained a well-formedness error that XML requires be detected and reported to the application),
            // then the task that is queued on the networking task source in which the aforementioned problem is found must change the text track readiness state to failed to
            // load and fire an event named error at the track element.
            // FIXME: Currently we always fail here, since we don't support loading any track formats.
            queue_an_element_task(Task::Source::Networking, [this, &realm]() {
                m_track->set_readiness_state(TextTrack::ReadinessState::FailedToLoad);
                dispatch_event(DOM::Event::create(realm, HTML::EventNames::error));
            });

            // If fetching does not fail, and the file was successfully processed, then the final task that is queued by the networking task source,
            // after it has finished parsing the data, must change the text track readiness state to loaded, and fire an event named load at the track element.
            // FIXME: Enable this once we support processing track files
            if (false) {
                queue_an_element_task(Task::Source::Networking, [this, &realm]() {
                    m_track->set_readiness_state(TextTrack::ReadinessState::Loaded);
                    dispatch_event(DOM::Event::create(realm, HTML::EventNames::load));
                });
            }
        };

        // 4. Fetch request.
        m_fetch_algorithms = Fetch::Infrastructure::FetchAlgorithms::create(vm(), move(fetch_algorithms_input));
        m_fetch_controller = MUST(Fetch::Fetching::fetch(realm, request, *m_fetch_algorithms));
    } else {
        fire_error_event();
        return;
    }

    // 11. Wait until the text track readiness state is no longer set to loading.
    HTML::main_thread_event_loop().spin_until(GC::create_function(realm.heap(), [this] {
        return m_track->readiness_state() != TextTrack::ReadinessState::Loading;
    }));

    // 12. Wait until the track URL is no longer equal to URL, at the same time as the text track mode is set to hidden or showing.
    HTML::main_thread_event_loop().spin_until(GC::create_function(realm.heap(), [this, url = move(url)] {
        return track_url() != url && first_is_one_of(m_track->mode(), Bindings::TextTrackMode::Hidden, Bindings::TextTrackMode::Showing);
    }));

    // 13. Jump to the step labeled top.
    start_the_track_processing_model_parallel_steps(realm);
}

}

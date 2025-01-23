/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Promise.h>
#include <LibMedia/Audio/Loader.h>
#include <LibMedia/PlaybackManager.h>
#include <LibWeb/Bindings/HTMLMediaElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWeb/HTML/AudioTrack.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLSourceElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/MediaError.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TextTrackList.h>
#include <LibWeb/HTML/TimeRanges.h>
#include <LibWeb/HTML/TrackEvent.h>
#include <LibWeb/HTML/VideoTrack.h>
#include <LibWeb/HTML/VideoTrackList.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::HTML {

HTMLMediaElement::HTMLMediaElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLMediaElement::~HTMLMediaElement() = default;

void HTMLMediaElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLMediaElement);

    m_audio_tracks = realm.create<AudioTrackList>(realm);
    m_video_tracks = realm.create<VideoTrackList>(realm);
    m_text_tracks = realm.create<TextTrackList>(realm);
    m_document_observer = realm.create<DOM::DocumentObserver>(realm, document());

    // https://html.spec.whatwg.org/multipage/media.html#playing-the-media-resource:media-element-82
    m_document_observer->set_document_became_inactive([this]() {
        // If the media element's node document stops being a fully active document, then the playback will stop until
        // the document is active again.
        pause_element().release_value_but_fixme_should_propagate_errors();
    });

    document().page().register_media_element({}, unique_id());
}

void HTMLMediaElement::finalize()
{
    Base::finalize();
    document().page().unregister_media_element({}, unique_id());
}

// https://html.spec.whatwg.org/multipage/media.html#queue-a-media-element-task
void HTMLMediaElement::queue_a_media_element_task(Function<void()> steps)
{
    // To queue a media element task with a media element element and a series of steps steps, queue an element task on the media element's
    // media element event task source given element and steps.
    queue_an_element_task(media_element_event_task_source(), move(steps));
}

void HTMLMediaElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_error);
    visitor.visit(m_audio_tracks);
    visitor.visit(m_video_tracks);
    visitor.visit(m_text_tracks);
    visitor.visit(m_document_observer);
    visitor.visit(m_source_element_selector);
    visitor.visit(m_fetch_controller);
    visitor.visit(m_pending_play_promises);
}

void HTMLMediaElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::src) {
        load_element().release_value_but_fixme_should_propagate_errors();
    } else if (name == HTML::AttributeNames::crossorigin) {
        m_crossorigin = cors_setting_attribute_from_keyword(value);
    } else if (name == HTML::AttributeNames::muted) {
        set_muted(true);
    }
}

// https://html.spec.whatwg.org/multipage/media.html#playing-the-media-resource:media-element-83
void HTMLMediaElement::removed_from(DOM::Node* old_parent, DOM::Node& old_root)
{
    Base::removed_from(old_parent, old_root);

    // When a media element is removed from a Document, the user agent must run the following steps:

    // FIXME: 1. Await a stable state, allowing the task that removed the media element from the Document to continue. The
    //           synchronous section consists of all the remaining steps of this algorithm. (Steps in the synchronous section
    //           are marked with ⌛.)

    // 2. ⌛ If the media element is in a document, return.
    if (in_a_document_tree())
        return;

    // 3. ⌛ Run the internal pause steps for the media element.
    pause_element().release_value_but_fixme_should_propagate_errors();
}

// https://html.spec.whatwg.org/multipage/media.html#fatal-decode-error
void HTMLMediaElement::set_decoder_error(String error_message)
{
    auto& realm = this->realm();

    // -> If the media data is corrupted
    // Fatal errors in decoding the media data that occur after the user agent has established whether the current media
    // resource is usable (i.e. once the media element's readyState attribute is no longer HAVE_NOTHING) must cause the
    // user agent to execute the following steps:
    if (m_ready_state == ReadyState::HaveNothing)
        return;

    // 1. The user agent should cancel the fetching process.
    if (m_fetch_controller)
        m_fetch_controller->stop_fetch();

    // 2. Set the error attribute to the result of creating a MediaError with MEDIA_ERR_DECODE.
    m_error = realm.create<MediaError>(realm, MediaError::Code::Decode, move(error_message));

    // 3. Set the element's networkState attribute to the NETWORK_IDLE value.
    m_network_state = NetworkState::Idle;

    // 4. Set the element's delaying-the-load-event flag to false. This stops delaying the load event.
    m_delaying_the_load_event.clear();

    // 5. Fire an event named error at the media element.
    dispatch_event(DOM::Event::create(realm, HTML::EventNames::error));

    // FIXME: 6. Abort the overall resource selection algorithm.
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-buffered
GC::Ref<TimeRanges> HTMLMediaElement::buffered() const
{
    auto& realm = this->realm();

    // FIXME: The buffered attribute must return a new static normalized TimeRanges object that represents the ranges of the
    //        media resource, if any, that the user agent has buffered, at the time the attribute is evaluated. Users agents
    //        must accurately determine the ranges available, even for media streams where this can only be determined by
    //        tedious inspection.
    return realm.create<TimeRanges>(realm);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-navigator-canplaytype
Bindings::CanPlayTypeResult HTMLMediaElement::can_play_type(StringView type) const
{
    // The canPlayType(type) method must:
    // - return the empty string if type is a type that the user agent knows it cannot render or is the type "application/octet-stream"
    // - return "probably" if the user agent is confident that the type represents a media resource that it can render if used in with this audio or video element
    // - return "maybe" otherwise. Implementers are encouraged to return "maybe" unless the type can be confidently established as being supported or not
    // Generally, a user agent should never return "probably" for a type that allows the codecs parameter if that parameter is not present.
    if (type == "application/octet-stream"sv)
        return Bindings::CanPlayTypeResult::Empty;

    auto mime_type = MimeSniff::MimeType::parse(type);

    if (mime_type.has_value() && mime_type->type() == "video"sv) {
        if (supported_video_subtypes.contains_slow(mime_type->subtype()))
            return Bindings::CanPlayTypeResult::Probably;
        return Bindings::CanPlayTypeResult::Maybe;
    }

    if (mime_type.has_value() && mime_type->type() == "audio"sv) {
        auto result = Bindings::CanPlayTypeResult::Maybe;
        if (supported_audio_subtypes.contains_slow(mime_type->subtype()))
            result = Bindings::CanPlayTypeResult::Probably;

        // "Maybe" because we support mp3, but "mpeg" can also refer to MP1 and MP2.
        if (mime_type->subtype() == "mpeg"sv)
            result = Bindings::CanPlayTypeResult::Maybe;

        return result;
    }

    return Bindings::CanPlayTypeResult::Empty;
}

void HTMLMediaElement::set_seeking(bool seeking)
{
    if (m_seeking == seeking)
        return;
    m_seeking = seeking;
    set_needs_style_update(true);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-load
WebIDL::ExceptionOr<void> HTMLMediaElement::load()
{
    // When the load() method on a media element is invoked, the user agent must run the media element load algorithm.
    TRY(load_element());
    return {};
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-currenttime
double HTMLMediaElement::current_time() const
{
    // The currentTime attribute must, on getting, return the media element's default playback start position, unless that is zero,
    // in which case it must return the element's official playback position. The returned value must be expressed in seconds.
    if (m_default_playback_start_position != 0)
        return m_default_playback_start_position;
    return m_official_playback_position;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-currenttime
void HTMLMediaElement::set_current_time(double current_time)
{
    // On setting, if the media element's readyState is HAVE_NOTHING, then it must set the media element's default playback start
    // position to the new value; otherwise, it must set the official playback position to the new value and then seek to the new
    // value. The new value must be interpreted as being in seconds.
    if (m_ready_state == ReadyState::HaveNothing) {
        m_default_playback_start_position = current_time;
    } else {
        m_official_playback_position = current_time;
        seek_element(current_time);
    }
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-fastseek
void HTMLMediaElement::fast_seek(double time)
{
    // The fastSeek(time) method must seek to the time given by time, with the approximate-for-speed flag set.
    seek_element(time, MediaSeekMode::ApproximateForSpeed);
}

// https://html.spec.whatwg.org/multipage/media.html#playing-the-media-resource:current-playback-position-13
void HTMLMediaElement::set_current_playback_position(double playback_position)
{
    // When the current playback position of a media element changes (e.g. due to playback or seeking), the user agent must
    // run the time marches on steps. To support use cases that depend on the timing accuracy of cue event firing, such as
    // synchronizing captions with shot changes in a video, user agents should fire cue events as close as possible to their
    // position on the media timeline, and ideally within 20 milliseconds. If the current playback position changes while the
    // steps are running, then the user agent must wait for the steps to complete, and then must immediately rerun the steps.
    // These steps are thus run as often as possible or needed.
    // FIXME: Detect "the current playback position changes while the steps are running".
    m_current_playback_position = playback_position;

    // FIXME: Regarding the official playback position, the spec states:
    //
    //        Any time the user agent provides a stable state, the official playback position must be set to the current playback position.
    //        https://html.spec.whatwg.org/multipage/media.html#playing-the-media-resource:official-playback-position-2
    //
    //        We do not currently have a means to track a "stable state", so for now, keep the official playback position
    //        in sync with the current playback position.
    m_official_playback_position = m_current_playback_position;

    time_marches_on();

    // NOTE: This notifies blocked seek_element() invocations that we have finished seeking.
    m_seek_in_progress = false;

    // NOTE: Invoking the following steps is not listed in the spec. Rather, the spec just describes the scenario in
    //       which these steps should be invoked, which is when we've reached the end of the media playback.
    if (m_current_playback_position == m_duration)
        reached_end_of_media_playback();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-duration
double HTMLMediaElement::duration() const
{
    // The duration attribute must return the time of the end of the media resource, in seconds, on the media timeline. If no media data is available,
    // then the attributes must return the Not-a-Number (NaN) value. If the media resource is not known to be bounded (e.g. streaming radio, or a live
    // event with no announced end time), then the attribute must return the positive Infinity value.

    // FIXME: Handle unbounded media resources.
    return m_duration;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-ended
bool HTMLMediaElement::ended() const
{
    // The ended attribute must return true if, the last time the event loop reached step 1, the media element had ended
    // playback and the direction of playback was forwards, and false otherwise.
    // FIXME: Add a hook into EventLoop::process() to be notified when step 1 is reached.
    // FIXME: Detect playback direction.
    return has_ended_playback();
}

// https://html.spec.whatwg.org/multipage/media.html#durationChange
void HTMLMediaElement::set_duration(double duration)
{
    // When the length of the media resource changes to a known value (e.g. from being unknown to known, or from a previously established length to a new
    // length) the user agent must queue a media element task given the media element to fire an event named durationchange at the media element. (The event
    // is not fired when the duration is reset as part of loading a new media resource.) If the duration is changed such that the current playback position
    // ends up being greater than the time of the end of the media resource, then the user agent must also seek to the time of the end of the media resource.
    if (!isnan(duration)) {
        queue_a_media_element_task([this] {
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::durationchange));
        });

        if (m_current_playback_position > duration)
            seek_element(duration);
    }

    m_duration = duration;

    if (auto* paintable = this->paintable())
        paintable->set_needs_display();
}

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> HTMLMediaElement::play()
{
    auto& realm = this->realm();

    // FIXME: 1. If the media element is not allowed to play, then return a promise rejected with a "NotAllowedError" DOMException.

    // 2. If the media element's error attribute is not null and its code is MEDIA_ERR_SRC_NOT_SUPPORTED, then return a promise
    //    rejected with a "NotSupportedError" DOMException.
    if (m_error && m_error->code() == MediaError::Code::SrcNotSupported) {
        auto exception = WebIDL::NotSupportedError::create(realm, m_error->message());
        return WebIDL::create_rejected_promise_from_exception(realm, move(exception));
    }

    // 3. Let promise be a new promise and append promise to the list of pending play promises.
    auto promise = WebIDL::create_promise(realm);
    m_pending_play_promises.append(promise);

    // 4. Run the internal play steps for the media element.
    TRY(play_element());

    // 5. Return promise.
    return promise;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-pause
WebIDL::ExceptionOr<void> HTMLMediaElement::pause()
{
    // 1. If the media element's networkState attribute has the value NETWORK_EMPTY, invoke the media element's resource
    //    selection algorithm.
    if (m_network_state == NetworkState::Empty)
        TRY(select_resource());

    // 2. Run the internal pause steps for the media element.
    TRY(pause_element());

    return {};
}

WebIDL::ExceptionOr<void> HTMLMediaElement::toggle_playback()
{
    // AD-HOC: An execution context is required for Promise creation hooks.
    TemporaryExecutionContext execution_context { realm() };

    if (potentially_playing())
        TRY(pause());
    else
        TRY(play());

    return {};
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-volume
WebIDL::ExceptionOr<void> HTMLMediaElement::set_volume(double volume)
{
    if (m_volume == volume)
        return {};

    // On setting, if the new value is in the range 0.0 to 1.0 inclusive, the media element's playback volume must be
    // set to the new value. If the new value is outside the range 0.0 to 1.0 inclusive, then, on setting, an
    // "IndexSizeError" DOMException must be thrown instead.
    if (volume < 0.0 || volume > 1.0)
        return WebIDL::IndexSizeError::create(realm(), "Volume must be in the range 0.0 to 1.0, inclusive"_string);

    m_volume = volume;
    volume_or_muted_attribute_changed();

    return {};
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-muted
void HTMLMediaElement::set_muted(bool muted)
{
    if (m_muted == muted)
        return;

    m_muted = muted;
    volume_or_muted_attribute_changed();
    set_needs_style_update(true);
}

// https://html.spec.whatwg.org/multipage/media.html#user-interface:dom-media-volume-3
void HTMLMediaElement::volume_or_muted_attribute_changed()
{
    // Whenever either of the values that would be returned by the volume and muted IDL attributes change, the user
    // agent must queue a media element task given the media element to fire an event named volumechange at the media
    // element.
    queue_a_media_element_task([this] {
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::volumechange));
    });

    // FIXME: Then, if the media element is not allowed to play, the user agent must run the internal pause steps for the media element.

    if (auto* paintable = this->paintable())
        paintable->set_needs_display();

    on_volume_change();
}

void HTMLMediaElement::page_mute_state_changed(Badge<Page>)
{
    on_volume_change();
}

// https://html.spec.whatwg.org/multipage/media.html#effective-media-volume
double HTMLMediaElement::effective_media_volume() const
{
    // 1. If the user has indicated that the user agent is to override the volume of the element, then return the
    //    volume desired by the user.
    if (document().page().page_mute_state() == MuteState::Muted)
        return 0.0;

    // 2. If the element's audio output is muted, then return zero.
    if (m_muted)
        return 0.0;

    // 3. Let volume be the playback volume of the audio portions of the media element, in range 0.0 (silent) to
    //    1.0 (loudest).
    auto volume = clamp(m_volume, 0.0, 1.0);

    // 4. Return volume, interpreted relative to the range 0.0 to 1.0, with 0.0 being silent, and 1.0 being the loudest
    //    setting, values in between increasing in loudness. The range need not be linear. The loudest setting may be
    //    lower than the system's loudest possible setting; for example the user could have set a maximum volume.
    return volume;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-addtexttrack
GC::Ref<TextTrack> HTMLMediaElement::add_text_track(Bindings::TextTrackKind kind, String const& label, String const& language)
{
    // 1. Create a new TextTrack object.
    auto text_track = TextTrack::create(this->realm());

    // 2. Create a new text track corresponding to the new object, and set its text track kind to kind, its text track
    //    label to label, its text track language to language, its text track readiness state to the text track loaded
    //    state, its text track mode to the text track hidden mode, and its text track list of cues to an empty list.
    text_track->set_kind(kind);
    text_track->set_label(label);
    text_track->set_language(language);
    text_track->set_readiness_state(TextTrack::ReadinessState::Loaded);
    text_track->set_mode(Bindings::TextTrackMode::Hidden);
    // FIXME: set text track list of cues to an empty list

    // FIXME: 3. Initially, the text track list of cues is not associated with any rules for updating the text track rendering.
    //    When a text track cue is added to it, the text track list of cues has its rules permanently set accordingly.

    // FIXME: 4. Add the new text track to the media element's list of text tracks.

    // 5. Queue a media element task given the media element to fire an event named addtrack at the media element's
    //    textTracks attribute's TextTrackList object, using TrackEvent, with the track attribute initialized to the new
    //    text track's TextTrack object.
    queue_a_media_element_task([this, text_track] {
        TrackEventInit event_init {};
        event_init.track = GC::make_root(text_track);

        auto event = TrackEvent::create(this->realm(), HTML::EventNames::addtrack, move(event_init));
        m_text_tracks->dispatch_event(event);
    });

    // 6. Return the new TextTrack object.
    return text_track;
}

// https://html.spec.whatwg.org/multipage/media.html#media-element-load-algorithm
WebIDL::ExceptionOr<void> HTMLMediaElement::load_element()
{
    m_first_data_load_event_since_load_start = true;

    // FIXME: 1. Abort any already-running instance of the resource selection algorithm for this element.

    // 2. Let pending tasks be a list of all tasks from the media element's media element event task source in one of the task queues.
    [[maybe_unused]] auto pending_tasks = HTML::main_thread_event_loop().task_queue().take_tasks_matching([&](auto& task) {
        return task.source() == media_element_event_task_source();
    });

    // FIXME: 3. For each task in pending tasks that would resolve pending play promises or reject pending play promises, immediately resolve or
    //           reject those promises in the order the corresponding tasks were queued.

    // 4. Remove each task in pending tasks from its task queue
    //    NOTE: We performed this step along with step 2.

    // 5. If the media element's networkState is set to NETWORK_LOADING or NETWORK_IDLE, queue a media element task given the media element to
    //    fire an event named abort at the media element.
    if (m_network_state == NetworkState::Loading || m_network_state == NetworkState::Idle) {
        queue_a_media_element_task([this] {
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::abort));
        });
    }

    // 6. If the media element's networkState is not set to NETWORK_EMPTY, then:
    if (m_network_state != NetworkState::Empty) {
        // 1. Queue a media element task given the media element to fire an event named emptied at the media element.
        queue_a_media_element_task([this] {
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::emptied));
        });

        // 2. If a fetching process is in progress for the media element, the user agent should stop it.
        if (m_fetch_controller && m_fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Ongoing)
            m_fetch_controller->stop_fetch();

        // FIXME: 3. If the media element's assigned media provider object is a MediaSource object, then detach it.

        // 4. Forget the media element's media-resource-specific tracks.
        forget_media_resource_specific_tracks();

        // 5. If readyState is not set to HAVE_NOTHING, then set it to that state.
        if (m_ready_state != ReadyState::HaveNothing)
            set_ready_state(ReadyState::HaveNothing);

        // 6. If the paused attribute is false, then:
        if (!paused()) {
            // 1. Set the paused attribute to true.
            set_paused(true);

            // 2. Take pending play promises and reject pending play promises with the result and an "AbortError" DOMException.
            auto promises = take_pending_play_promises();
            reject_pending_play_promises<WebIDL::AbortError>(promises, "Media playback was aborted"_string);
        }

        // 7. If seeking is true, set it to false.
        if (seeking())
            set_seeking(false);

        // 8. Set the current playback position to 0.
        m_current_playback_position = 0;

        if (m_official_playback_position != 0) {
            // Set the official playback position to 0.
            m_official_playback_position = 0;

            // If this changed the official playback position, then queue a media element task given the media element to fire an
            // event named timeupdate at the media element.
            queue_a_media_element_task([this] {
                dispatch_time_update_event();
            });
        }

        // FIXME: 9. Set the timeline offset to Not-a-Number (NaN).

        // 10. Update the duration attribute to Not-a-Number (NaN).
        set_duration(NAN);
    }

    // FIXME: 7. Set the playbackRate attribute to the value of the defaultPlaybackRate attribute.

    // 8. Set the error attribute to null and the can autoplay flag to true.
    m_error = nullptr;
    m_can_autoplay = true;

    // 9. Invoke the media element's resource selection algorithm.
    TRY(select_resource());

    // 10. NOTE: Playback of any previously playing media resource for this element stops.
    return {};
}

enum class SelectMode {
    Object,
    Attribute,
    Children,
};

class SourceElementSelector final : public JS::Cell {
    GC_CELL(SourceElementSelector, JS::Cell);
    GC_DECLARE_ALLOCATOR(SourceElementSelector);

public:
    SourceElementSelector(GC::Ref<HTMLMediaElement> media_element, GC::Ref<HTMLSourceElement> candidate)
        : m_media_element(media_element)
        , m_candidate(candidate)
    {
    }

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_media_element);
        visitor.visit(m_candidate);
        visitor.visit(m_previously_failed_candidate);
    }

    WebIDL::ExceptionOr<void> process_candidate()
    {
        // 2. ⌛ Process candidate: If candidate does not have a src attribute, or if its src attribute's value is the
        //    empty string, then end the synchronous section, and jump down to the failed with elements step below.
        String candiate_src;
        if (auto maybe_src = m_candidate->get_attribute(HTML::AttributeNames::src); maybe_src.has_value())
            candiate_src = *maybe_src;

        if (candiate_src.is_empty()) {
            TRY(failed_with_elements());
            return {};
        }

        // 3. ⌛ Let urlString and urlRecord be the resulting URL string and the resulting URL record, respectively, that
        //    would have resulted from parsing the URL specified by candidate's src attribute's value relative to the
        //    candidate's node document when the src attribute was last changed.
        auto url_record = m_candidate->document().parse_url(candiate_src);

        // 4. ⌛ If urlString was not obtained successfully, then end the synchronous section, and jump down to the failed
        //    with elements step below.
        if (!url_record.has_value()) {
            TRY(failed_with_elements());
            return {};
        }
        auto url_string = url_record->to_string();

        // FIXME: 5. ⌛ If candidate has a type attribute whose value, when parsed as a MIME type (including any codecs described
        //           by the codecs parameter, for types that define that parameter), represents a type that the user agent knows
        //           it cannot render, then end the synchronous section, and jump down to the failed with elements step below.

        // 6. ⌛ Set the currentSrc attribute to urlString.
        m_media_element->m_current_src = move(url_string);

        // 7. End the synchronous section, continuing the remaining steps in parallel.

        // 8. Run the resource fetch algorithm with urlRecord. If that algorithm returns without aborting this one, then
        //    the load failed.
        TRY(m_media_element->fetch_resource(*url_record, [this](auto) {
            failed_with_elements().release_value_but_fixme_should_propagate_errors();
        }));

        return {};
    }

    WebIDL::ExceptionOr<void> process_next_candidate()
    {
        if (!m_previously_failed_candidate)
            return {};

        TRY(wait_for_next_candidate(*m_previously_failed_candidate));
        return {};
    }

private:
    WebIDL::ExceptionOr<void> failed_with_elements()
    {
        // 9. Failed with elements: Queue a media element task given the media element to fire an event named error at candidate.
        m_media_element->queue_a_media_element_task([this]() {
            m_candidate->dispatch_event(DOM::Event::create(m_candidate->realm(), HTML::EventNames::error));
        });

        // FIXME: 10. Await a stable state. The synchronous section consists of all the remaining steps of this algorithm until
        //            the algorithm says the synchronous section has ended. (Steps in synchronous sections are marked with ⌛.)

        // 11. ⌛ Forget the media element's media-resource-specific tracks.
        m_media_element->forget_media_resource_specific_tracks();

        TRY(find_next_candidate(m_candidate));
        return {};
    }

    WebIDL::ExceptionOr<void> find_next_candidate(GC::Ref<DOM::Node> previous_candidate)
    {
        // 12. ⌛ Find next candidate: Let candidate be null.
        GC::Ptr<HTMLSourceElement> candidate;

        // 13. ⌛ Search loop: If the node after pointer is the end of the list, then jump to the waiting step below.
        auto* next_sibling = previous_candidate->next_sibling();
        if (!next_sibling) {
            TRY(waiting(previous_candidate));
            return {};
        }

        // 14. ⌛ If the node after pointer is a source element, let candidate be that element.
        if (is<HTMLSourceElement>(next_sibling))
            candidate = static_cast<HTMLSourceElement*>(next_sibling);

        // 15. ⌛ Advance pointer so that the node before pointer is now the node that was after pointer, and the node
        //     after pointer is the node after the node that used to be after pointer, if any.

        // 16. ⌛ If candidate is null, jump back to the search loop step. Otherwise, jump back to the process candidate step.
        if (!candidate) {
            TRY(find_next_candidate(*next_sibling));
            return {};
        }

        m_candidate = *candidate;
        TRY(process_candidate());

        return {};
    }

    WebIDL::ExceptionOr<void> waiting(GC::Ref<DOM::Node> previous_candidate)
    {
        // 17. ⌛ Waiting: Set the element's networkState attribute to the NETWORK_NO_SOURCE value.
        m_media_element->m_network_state = HTMLMediaElement::NetworkState::NoSource;

        // 18. ⌛ Set the element's show poster flag to true.
        m_media_element->set_show_poster(true);

        // 19. ⌛ Queue a media element task given the media element to set the element's delaying-the-load-event flag
        //     to false. This stops delaying the load event.
        m_media_element->queue_a_media_element_task([this]() {
            m_media_element->m_delaying_the_load_event.clear();
        });

        // 20. End the synchronous section, continuing the remaining steps in parallel.

        // 21. Wait until the node after pointer is a node other than the end of the list. (This step might wait forever.)
        TRY(wait_for_next_candidate(previous_candidate));

        return {};
    }

    WebIDL::ExceptionOr<void> wait_for_next_candidate(GC::Ref<DOM::Node> previous_candidate)
    {
        // NOTE: If there isn't another candidate to check, we implement the "waiting" step by returning until the media
        //       element's children have changed.
        if (previous_candidate->next_sibling() == nullptr) {
            m_previously_failed_candidate = previous_candidate;
            return {};
        }

        m_previously_failed_candidate = nullptr;

        // FIXME: 22. Await a stable state. The synchronous section consists of all the remaining steps of this algorithm until
        //            the algorithm says the synchronous section has ended. (Steps in synchronous sections are marked with ⌛.)

        // 23. ⌛ Set the element's delaying-the-load-event flag back to true (this delays the load event again, in case
        //     it hasn't been fired yet).
        m_media_element->m_delaying_the_load_event.emplace(m_media_element->document());

        // 24. ⌛ Set the networkState back to NETWORK_LOADING.
        m_media_element->m_network_state = HTMLMediaElement::NetworkState::Loading;

        // 25. ⌛ Jump back to the find next candidate step above.
        TRY(find_next_candidate(previous_candidate));

        return {};
    }

    GC::Ref<HTMLMediaElement> m_media_element;
    GC::Ref<HTMLSourceElement> m_candidate;
    GC::Ptr<DOM::Node> m_previously_failed_candidate;
};

GC_DEFINE_ALLOCATOR(SourceElementSelector);

void HTMLMediaElement::children_changed()
{
    if (m_source_element_selector)
        m_source_element_selector->process_next_candidate().release_value_but_fixme_should_propagate_errors();
}

// https://html.spec.whatwg.org/multipage/media.html#concept-media-load-algorithm
WebIDL::ExceptionOr<void> HTMLMediaElement::select_resource()
{
    auto& realm = this->realm();

    // 1. Set the element's networkState attribute to the NETWORK_NO_SOURCE value.
    m_network_state = NetworkState::NoSource;

    // 2. Set the element's show poster flag to true.
    set_show_poster(true);

    // 3. Set the media element's delaying-the-load-event flag to true (this delays the load event).
    m_delaying_the_load_event.emplace(document());

    // FIXME: 4. Await a stable state, allowing the task that invoked this algorithm to continue. The synchronous section consists of all the remaining
    //           steps of this algorithm until the algorithm says the synchronous section has ended. (Steps in synchronous sections are marked with ⌛.)

    // FIXME: 5. ⌛ If the media element's blocked-on-parser flag is false, then populate the list of pending text tracks.

    Optional<SelectMode> mode;
    GC::Ptr<HTMLSourceElement> candidate;

    // 6. FIXME: ⌛ If the media element has an assigned media provider object, then let mode be object.

    // ⌛ Otherwise, if the media element has no assigned media provider object but has a src attribute, then let mode be attribute.
    if (has_attribute(HTML::AttributeNames::src)) {
        mode = SelectMode::Attribute;
    }
    // ⌛ Otherwise, if the media element does not have an assigned media provider object and does not have a src attribute, but does have
    // a source element child, then let mode be children and let candidate be the first such source element child in tree order.
    else if (auto* source_element = first_child_of_type<HTMLSourceElement>()) {
        mode = SelectMode::Children;
        candidate = source_element;
    }
    // ⌛ Otherwise the media element has no assigned media provider object and has neither a src attribute nor a source element child:
    else {
        // 1. ⌛ Set the networkState to NETWORK_EMPTY.
        m_network_state = NetworkState::Empty;

        // 2. ⌛ Set the element's delaying-the-load-event flag to false. This stops delaying the load event.
        m_delaying_the_load_event.clear();

        // 3. End the synchronous section and return.
        return {};
    }

    // 7. ⌛ Set the media element's networkState to NETWORK_LOADING.
    m_network_state = NetworkState::Loading;

    // 8. ⌛ Queue a media element task given the media element to fire an event named loadstart at the media element.
    queue_a_media_element_task([this] {
        dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::loadstart));
    });

    // 9. Run the appropriate steps from the following list:
    switch (*mode) {
    // -> If mode is object
    case SelectMode::Object:
        // FIXME: 1. ⌛ Set the currentSrc attribute to the empty string.
        // FIXME: 2. End the synchronous section, continuing the remaining steps in parallel.
        // FIXME: 3. Run the resource fetch algorithm with the assigned media provider object. If that algorithm returns without aborting this one,
        //           then theload failed.
        // FIXME: 4. Failed with media provider: Reaching this step indicates that the media resource failed to load. Take pending play promises and queue
        //           a media element task given the media element to run the dedicated media source failure steps with the result.
        // FIXME: 5. Wait for the task queued by the previous step to have executed.

        // 6. Return. The element won't attempt to load another resource until this algorithm is triggered again.
        return {};

    // -> If mode is attribute
    case SelectMode::Attribute: {
        auto failed_with_attribute = [this](auto error_message) {
            IGNORE_USE_IN_ESCAPING_LAMBDA bool ran_media_element_task = false;

            // 6. Failed with attribute: Reaching this step indicates that the media resource failed to load or that the given URL could not be parsed. Take
            //    pending play promises and queue a media element task given the media element to run the dedicated media source failure steps with the result.
            queue_a_media_element_task([this, &ran_media_element_task, error_message = move(error_message)]() mutable {
                auto promises = take_pending_play_promises();
                handle_media_source_failure(promises, move(error_message)).release_value_but_fixme_should_propagate_errors();

                ran_media_element_task = true;
            });

            // 7. Wait for the task queued by the previous step to have executed.
            HTML::main_thread_event_loop().spin_until(GC::create_function(heap(), [&]() { return ran_media_element_task; }));
        };

        // 1. ⌛ If the src attribute's value is the empty string, then end the synchronous section, and jump down to the failed with attribute step below.
        auto source = get_attribute_value(HTML::AttributeNames::src);
        if (source.is_empty()) {
            failed_with_attribute("The 'src' attribute is empty"_string);
            return {};
        }

        // 2. ⌛ Let urlString and urlRecord be the resulting URL string and the resulting URL record, respectively, that would have resulted from parsing
        //    the URL specified by the src attribute's value relative to the media element's node document when the src attribute was last changed.
        auto url_record = document().parse_url(source);

        // 3. ⌛ If urlString was obtained successfully, set the currentSrc attribute to urlString.
        if (url_record.has_value())
            m_current_src = url_record->to_string();

        // 4. End the synchronous section, continuing the remaining steps in parallel.

        // 5. If urlRecord was obtained successfully, run the resource fetch algorithm with urlRecord. If that algorithm returns without aborting this one,
        //    then the load failed.
        if (url_record.has_value()) {
            TRY(fetch_resource(*url_record, move(failed_with_attribute)));
            return {};
        }

        failed_with_attribute("Failed to parse 'src' attribute as a URL"_string);

        // 8. Return. The element won't attempt to load another resource until this algorithm is triggered again.
        return {};
    }

    // -> Otherwise (mode is children)
    case SelectMode::Children:
        VERIFY(candidate);

        // 1. ⌛ Let pointer be a position defined by two adjacent nodes in the media element's child list, treating the start of the list (before the
        //    first child in the list, if any) and end of the list (after the last child in the list, if any) as nodes in their own right. One node is
        //    the node before pointer, and the other node is the node after pointer. Initially, let pointer be the position between the candidate node
        //    and the next node, if there are any, or the end of the list, if it is the last node.
        //
        //    As nodes are inserted and removed into the media element, pointer must be updated as follows:
        //
        //    If a new node is inserted between the two nodes that define pointer
        //        Let pointer be the point between the node before pointer and the new node. In other words, insertions at pointer go after pointer.
        //    If the node before pointer is removed
        //        Let pointer be the point between the node after pointer and the node before the node after pointer. In other words, pointer doesn't
        //        move relative to the remaining nodes.
        //    If the node after pointer is removed
        //        Let pointer be the point between the node before pointer and the node after the node before pointer. Just as with the previous case,
        //        pointer doesn't move relative to the remaining nodes.
        //    Other changes don't affect pointer.

        // NOTE: We do not bother with maintaining this pointer. We inspect the DOM tree on the fly, rather than dealing
        //       with the headache of auto-updating this pointer as the DOM changes.

        m_source_element_selector = realm.create<SourceElementSelector>(*this, *candidate);
        TRY(m_source_element_selector->process_candidate());

        break;
    }

    return {};
}

enum class FetchMode {
    Local,
    Remote,
};

// https://html.spec.whatwg.org/multipage/media.html#concept-media-load-resource
WebIDL::ExceptionOr<void> HTMLMediaElement::fetch_resource(URL::URL const& url_record, Function<void(String)> failure_callback)
{
    auto& realm = this->realm();
    auto& vm = realm.vm();

    // 1. Let mode be remote.
    auto mode = FetchMode::Remote;

    // FIXME: 2. If the algorithm was invoked with media provider object, then set mode to local.
    //           Otherwise:
    //           1. Let object be the result of obtaining a blob object using the URL record's blob URL entry and the media
    //              element's node document's relevant settings object.
    //           2. If object is a media provider object, then set mode to local.
    // FIXME: 3. If mode is remote, then let the current media resource be the resource given by the URL record passed to this algorithm; otherwise, let the
    //           current media resource be the resource given by the media provider object. Either way, the current media resource is now the element's media
    //           resource.
    // FIXME: 4. Remove all media-resource-specific text tracks from the media element's list of pending text tracks, if any.

    // 5. Run the appropriate steps from the following list:
    switch (mode) {
    // -> If mode is remote
    case FetchMode::Remote: {
        // FIXME: 1. Optionally, run the following substeps. This is the expected behavior if the user agent intends to not attempt to fetch the resource until
        //           the user requests it explicitly (e.g. as a way to implement the preload attribute's none keyword).
        //            1. Set the networkState to NETWORK_IDLE.
        //            2. Queue a media element task given the media element to fire an event named suspend at the element.
        //            3. Queue a media element task given the media element to set the element's delaying-the-load-event flag to false. This stops delaying
        //               the load event.
        //            4. Wait for the task to be run.
        //            5. Wait for an implementation-defined event (e.g., the user requesting that the media element begin playback).
        //            6. Set the element's delaying-the-load-event flag back to true (this delays the load event again, in case it hasn't been fired yet).
        //            7. Set the networkState to NETWORK_LOADING.

        // 2. Let destination be "audio" if the media element is an audio element, or "video" otherwise.
        auto destination = is<HTMLAudioElement>(*this)
            ? Fetch::Infrastructure::Request::Destination::Audio
            : Fetch::Infrastructure::Request::Destination::Video;

        // 3. Let request be the result of creating a potential-CORS request given current media resource's URL record, destination, and the current state
        //    of media element's crossorigin content attribute.
        auto request = create_potential_CORS_request(vm, url_record, destination, m_crossorigin);

        // 4. Set request's client to the media element's node document's relevant settings object.
        request->set_client(&document().relevant_settings_object());

        // 5. Set request's initiator type to destination.
        request->set_initiator_type(destination == Fetch::Infrastructure::Request::Destination::Audio
                ? Fetch::Infrastructure::Request::InitiatorType::Audio
                : Fetch::Infrastructure::Request::InitiatorType::Video);

        // 6. Let byteRange, which is "entire resource" or a (number, number or "until end") tuple, be the byte range required to satisfy missing data in
        //    media data. This value is implementation-defined and may rely on codec, network conditions or other heuristics. The user-agent may determine
        //    to fetch the resource in full, in which case byteRange would be "entire resource", to fetch from a byte offset until the end, in which case
        //    byteRange would be (number, "until end"), or to fetch a range between two byte offsets, im which case byteRange would be a (number, number)
        //    tuple representing the two offsets.
        ByteRange byte_range = EntireResource {};

        // FIXME: 7. If byteRange is not "entire resource", then:
        //            1. If byteRange[1] is "until end" then add a range header to request given byteRange[0].
        //            2. Otherwise, add a range header to request given byteRange[0] and byteRange[1].

        // 8. Fetch request, with processResponse set to the following steps given response response:
        Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};

        fetch_algorithms_input.process_response = [this, byte_range = move(byte_range), failure_callback = move(failure_callback)](auto response) mutable {
            auto& realm = this->realm();

            // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
            //        https://github.com/whatwg/html/issues/9355
            response = response->unsafe_response();

            // 1. Let global be the media element's node document's relevant global object.
            auto& global = document().realm().global_object();

            // 4. If the result of verifying response given the current media resource and byteRange is false, then abort these steps.
            // NOTE: We do this step before creating the updateMedia task so that we can invoke the failure callback.
            if (!verify_response(response, byte_range)) {
                auto error_message = response->network_error_message().value_or("Failed to fetch media resource"sv);
                failure_callback(String::from_utf8(error_message).release_value_but_fixme_should_propagate_errors());
                return;
            }

            // 2. Let updateMedia be to queue a media element task given the media element to run the first appropriate steps from the media data processing
            //    steps list below. (A new task is used for this so that the work described below occurs relative to the appropriate media element event task
            //    source rather than using the networking task source.)
            auto update_media = GC::create_function(heap(), [this, failure_callback = move(failure_callback)](ByteBuffer media_data) mutable {
                // 6. Update the media data with the contents of response's unsafe response obtained in this fashion. response can be CORS-same-origin or
                //    CORS-cross-origin; this affects whether subtitles referenced in the media data are exposed in the API and, for video elements, whether
                //    a canvas gets tainted when the video is drawn on it.
                m_media_data = move(media_data);

                queue_a_media_element_task([this, failure_callback = move(failure_callback)]() mutable {
                    process_media_data(move(failure_callback)).release_value_but_fixme_should_propagate_errors();

                    // NOTE: The spec does not say exactly when to update the readyState attribute. Rather, it describes what
                    //       each step requires, and leaves it up to the user agent to determine when those requirements are
                    //       reached: https://html.spec.whatwg.org/multipage/media.html#ready-states
                    //
                    //       Since we fetch the entire response at once, if we reach here with successfully decoded video
                    //       metadata, we have satisfied the HAVE_ENOUGH_DATA requirements. This logic will of course need
                    //       to change if we fetch or process the media data in smaller chunks.
                    if (m_ready_state == ReadyState::HaveMetadata)
                        set_ready_state(ReadyState::HaveEnoughData);
                });
            });

            // FIXME: 3. Let processEndOfMedia be the following step: If the fetching process has completes without errors, including decoding the media data,
            //           and if all of the data is available to the user agent without network access, then, the user agent must move on to the final step below.
            //           This might never happen, e.g. when streaming an infinite resource such as web radio, or if the resource is longer than the user agent's
            //           ability to cache data.

            // 5. Otherwise, incrementally read response's body given updateMedia, processEndOfMedia, an empty algorithm, and global.

            VERIFY(response->body());
            auto empty_algorithm = GC::create_function(heap(), [](JS::Value) { });

            // FIXME: We are "fully" reading the response here, rather than "incrementally". Memory concerns aside, this should be okay for now as we are
            //        always setting byteRange to "entire resource". However, we should switch to incremental reads when that is implemented, and then
            //        implement the processEndOfMedia step.
            response->body()->fully_read(realm, update_media, empty_algorithm, GC::Ref { global });
        };

        m_fetch_controller = TRY(Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(vm, move(fetch_algorithms_input))));
        break;
    }

    // -> Otherwise (mode is local)
    case FetchMode::Local:
        // FIXME:
        // The resource described by the current media resource, if any, contains the media data. It is CORS-same-origin.
        //
        // If the current media resource is a raw data stream (e.g. from a File object), then to determine the format of the media resource, the user agent
        // must use the rules for sniffing audio and video specifically. Otherwise, if the data stream is pre-decoded, then the format is the format given
        // by the relevant specification.
        //
        // Whenever new data for the current media resource becomes available, queue a media element task given the media element to run the first appropriate
        // steps from the media data processing steps list below.
        //
        // When the current media resource is permanently exhausted (e.g. all the bytes of a Blob have been processed), if there were no decoding errors,
        // then the user agent must move on to the final step below. This might never happen, e.g. if the current media resource is a MediaStream.
        break;
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/media.html#verify-a-media-response
bool HTMLMediaElement::verify_response(GC::Ref<Fetch::Infrastructure::Response> response, ByteRange const& byte_range)
{
    // 1. If response is a network error, then return false.
    if (response->is_network_error())
        return false;

    // 2. If byteRange is "entire resource", then return true.
    if (byte_range.has<EntireResource>())
        return true;

    // 3. Let internalResponse be response's unsafe response.
    // 4. If internalResponse's status is 200, then return true.
    // 5. If internalResponse's status is not 206, then return false.
    // 6. If the result of extracting content-range values from internalResponse is failure, then return false.
    TODO();
}

// https://html.spec.whatwg.org/multipage/media.html#media-data-processing-steps-list
WebIDL::ExceptionOr<void> HTMLMediaElement::process_media_data(Function<void(String)> failure_callback)
{
    auto& realm = this->realm();

    auto audio_loader = Audio::Loader::create(m_media_data.bytes());
    auto playback_manager = Media::PlaybackManager::from_data(m_media_data);

    // -> If the media data cannot be fetched at all, due to network errors, causing the user agent to give up trying to fetch the resource
    // -> If the media data can be fetched but is found by inspection to be in an unsupported format, or can otherwise not be rendered at all
    if (audio_loader.is_error() && playback_manager.is_error()) {
        // 1. The user agent should cancel the fetching process.
        m_fetch_controller->stop_fetch();

        // 2. Abort this subalgorithm, returning to the resource selection algorithm.
        failure_callback(MUST(String::from_utf8(playback_manager.error().description())));

        return {};
    }

    GC::Ptr<AudioTrack> audio_track;
    GC::Ptr<VideoTrack> video_track;

    // -> If the media resource is found to have an audio track
    if (!audio_loader.is_error()) {
        // 1. Create an AudioTrack object to represent the audio track.
        audio_track = realm.create<AudioTrack>(realm, *this, audio_loader.release_value());

        // 2. Update the media element's audioTracks attribute's AudioTrackList object with the new AudioTrack object.
        m_audio_tracks->add_track({}, *audio_track);

        // 3. Let enable be unknown.
        auto enable = TriState::Unknown;

        // FIXME: 4. If either the media resource or the URL of the current media resource indicate a particular set of audio tracks to enable, or if
        //           the user agent has information that would facilitate the selection of specific audio tracks to improve the user's experience, then:
        //           if this audio track is one of the ones to enable, then set enable to true, otherwise, set enable to false.

        // 5. If enable is still unknown, then, if the media element does not yet have an enabled audio track, then set enable to true, otherwise,
        //    set enable to false.
        if (enable == TriState::Unknown)
            enable = m_audio_tracks->has_enabled_track() ? TriState::False : TriState::True;

        // 6. If enable is true, then enable this audio track, otherwise, do not enable this audio track.
        if (enable == TriState::True)
            audio_track->set_enabled(true);

        // 7. Fire an event named addtrack at this AudioTrackList object, using TrackEvent, with the track attribute initialized to the new AudioTrack object.
        TrackEventInit event_init {};
        event_init.track = GC::make_root(audio_track);

        auto event = TrackEvent::create(realm, HTML::EventNames::addtrack, move(event_init));
        m_audio_tracks->dispatch_event(event);
    }

    // -> If the media resource is found to have a video track
    if (!playback_manager.is_error()) {
        // 1. Create a VideoTrack object to represent the video track.
        video_track = realm.create<VideoTrack>(realm, *this, playback_manager.release_value());

        // 2. Update the media element's videoTracks attribute's VideoTrackList object with the new VideoTrack object.
        m_video_tracks->add_track({}, *video_track);

        // 3. Let enable be unknown.
        auto enable = TriState::Unknown;

        // FIXME: 4. If either the media resource or the URL of the current media resource indicate a particular set of video tracks to enable, or if
        //           the user agent has information that would facilitate the selection of specific video tracks to improve the user's experience, then:
        //           if this video track is the first such video track, then set enable to true, otherwise, set enable to false.

        // 5. If enable is still unknown, then, if the media element does not yet have a selected video track, then set enable to true, otherwise, set
        //    enable to false.
        if (enable == TriState::Unknown)
            enable = m_video_tracks->selected_index() == -1 ? TriState::True : TriState::False;

        // 6. If enable is true, then select this track and unselect any previously selected video tracks, otherwise, do not select this video track.
        //    If other tracks are unselected, then a change event will be fired.
        if (enable == TriState::True)
            video_track->set_selected(true);

        // 7. Fire an event named addtrack at this VideoTrackList object, using TrackEvent, with the track attribute initialized to the new VideoTrack object.
        TrackEventInit event_init {};
        event_init.track = GC::make_root(video_track);

        auto event = TrackEvent::create(realm, HTML::EventNames::addtrack, move(event_init));
        m_video_tracks->dispatch_event(event);
    }

    // -> Once enough of the media data has been fetched to determine the duration of the media resource, its dimensions, and other metadata
    if (audio_track != nullptr || video_track != nullptr) {
        // AD-HOC: After selecting a track, we do not need the source element selector anymore.
        m_source_element_selector = nullptr;

        // FIXME: 1. Establish the media timeline for the purposes of the current playback position and the earliest possible position, based on the media data.
        // FIXME: 2. Update the timeline offset to the date and time that corresponds to the zero time in the media timeline established in the previous step,
        //           if any. If no explicit time and date is given by the media resource, the timeline offset must be set to Not-a-Number (NaN).

        // 3. Set the current playback position and the official playback position to the earliest possible position.
        m_current_playback_position = 0;
        m_official_playback_position = 0;

        // 4. Update the duration attribute with the time of the last frame of the resource, if known, on the media timeline established above. If it is
        //    not known (e.g. a stream that is in principle infinite), update the duration attribute to the value positive Infinity.
        // FIXME: Handle unbounded media resources.
        // 5. For video elements, set the videoWidth and videoHeight attributes, and queue a media element task given the media element to fire an event
        //    named resize at the media element.
        if (video_track && is<HTMLVideoElement>(*this)) {
            auto duration = video_track ? video_track->duration() : audio_track->duration();
            set_duration(static_cast<double>(duration.to_milliseconds()) / 1000.0);

            auto& video_element = as<HTMLVideoElement>(*this);
            video_element.set_video_width(video_track->pixel_width());
            video_element.set_video_height(video_track->pixel_height());

            queue_a_media_element_task([this] {
                dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::resize));
            });
        } else {
            auto duration = audio_track ? audio_track->duration() : video_track->duration();
            set_duration(static_cast<double>(duration.to_milliseconds()) / 1000.0);
        }

        // 6. Set the readyState attribute to HAVE_METADATA.
        set_ready_state(ReadyState::HaveMetadata);

        // 7. Let jumped be false.
        [[maybe_unused]] auto jumped = false;

        // 8. If the media element's default playback start position is greater than zero, then seek to that time, and let jumped be true.
        if (m_default_playback_start_position > 0) {
            seek_element(m_default_playback_start_position);
            jumped = true;
        }

        // 9. Let the media element's default playback start position be zero.
        m_default_playback_start_position = 0;

        // FIXME: 10. Let the initial playback position be zero.
        // FIXME: 11. If either the media resource or the URL of the current media resource indicate a particular start time, then set the initial playback
        //            position to that time and, if jumped is still false, seek to that time.

        // 12. If there is no enabled audio track, then enable an audio track. This will cause a change event to be fired.
        if (audio_track && !m_audio_tracks->has_enabled_track())
            audio_track->set_enabled(true);

        // 13. If there is no selected video track, then select a video track. This will cause a change event to be fired.
        if (video_track && m_video_tracks->selected_index() == -1)
            video_track->set_selected(true);
    }

    // -> Once the entire media resource has been fetched (but potentially before any of it has been decoded)
    if (audio_track != nullptr || video_track != nullptr) {
        // Fire an event named progress at the media element.
        dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::progress));

        // Set the networkState to NETWORK_IDLE and fire an event named suspend at the media element.
        m_network_state = NetworkState::Idle;
        dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::suspend));

        // If the user agent ever discards any media data and then needs to resume the network activity to obtain it again, then it must queue a media
        // element task given the media element to set the networkState to NETWORK_LOADING.
    }

    // FIXME: -> If the connection is interrupted after some media data has been received, causing the user agent to give up trying to fetch the resource
    // FIXME: -> If the media data fetching process is aborted by the user
    // FIXME: -> If the media data can be fetched but has non-fatal errors or uses, in part, codecs that are unsupported, preventing the user agent from
    //           rendering the content completely correctly but not preventing playback altogether
    // FIXME: -> If the media resource is found to declare a media-resource-specific text track that the user agent supports

    return {};
}

// https://html.spec.whatwg.org/multipage/media.html#dedicated-media-source-failure-steps
WebIDL::ExceptionOr<void> HTMLMediaElement::handle_media_source_failure(Span<GC::Ref<WebIDL::Promise>> promises, String error_message)
{
    auto& realm = this->realm();

    // 1. Set the error attribute to the result of creating a MediaError with MEDIA_ERR_SRC_NOT_SUPPORTED.
    m_error = realm.create<MediaError>(realm, MediaError::Code::SrcNotSupported, move(error_message));

    // 2. Forget the media element's media-resource-specific tracks.
    forget_media_resource_specific_tracks();

    // 3. Set the element's networkState attribute to the NETWORK_NO_SOURCE value.
    m_network_state = NetworkState::NoSource;

    // 4. Set the element's show poster flag to true.
    set_show_poster(true);

    // 5. Fire an event named error at the media element.
    dispatch_event(DOM::Event::create(realm, HTML::EventNames::error));

    // 6. Reject pending play promises with promises and a "NotSupportedError" DOMException.
    reject_pending_play_promises<WebIDL::NotSupportedError>(promises, "Media is not supported"_string);

    // 7. Set the element's delaying-the-load-event flag to false. This stops delaying the load event.
    m_delaying_the_load_event.clear();

    return {};
}

// https://html.spec.whatwg.org/multipage/media.html#forget-the-media-element's-media-resource-specific-tracks
void HTMLMediaElement::forget_media_resource_specific_tracks()
{
    // When a media element is to forget the media element's media-resource-specific tracks, the user agent must remove from the media element's list
    // of text tracks all the media-resource-specific text tracks, then empty the media element's audioTracks attribute's AudioTrackList object, then
    // empty the media element's videoTracks attribute's VideoTrackList object. No events (in particular, no removetrack events) are fired as part of
    // this; the error and emptied events, fired by the algorithms that invoke this one, can be used instead.
    m_audio_tracks->remove_all_tracks({});
    m_video_tracks->remove_all_tracks({});
}

// https://html.spec.whatwg.org/multipage/media.html#ready-states:media-element-3
void HTMLMediaElement::set_ready_state(ReadyState ready_state)
{
    ScopeGuard guard { [&] {
        m_ready_state = ready_state;
        set_needs_style_update(true);
    } };

    // When the ready state of a media element whose networkState is not NETWORK_EMPTY changes, the user agent must
    // follow the steps given below:
    if (m_network_state == NetworkState::Empty)
        return;

    // 1. Apply the first applicable set of substeps from the following list:
    // -> If the previous ready state was HAVE_NOTHING, and the new ready state is HAVE_METADATA
    if (m_ready_state == ReadyState::HaveNothing && ready_state == ReadyState::HaveMetadata) {
        // Queue a media element task given the media element to fire an event named loadedmetadata at the element.
        queue_a_media_element_task([this] {
            dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::loadedmetadata));
        });

        return;
    }

    // -> If the previous ready state was HAVE_METADATA and the new ready state is HAVE_CURRENT_DATA or greater
    if (m_ready_state == ReadyState::HaveMetadata && ready_state >= ReadyState::HaveCurrentData) {
        // If this is the first time this occurs for this media element since the load() algorithm was last invoked, the user agent must queue a media
        // element task given the media element to fire an event named loadeddata at the element.
        if (m_first_data_load_event_since_load_start) {
            m_first_data_load_event_since_load_start = false;

            queue_a_media_element_task([this] {
                dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::loadeddata));
            });
        }

        // https://html.spec.whatwg.org/multipage/media.html#loading-the-media-resource:dom-media-readystate-4
        // Once the readyState attribute reaches HAVE_CURRENT_DATA, after the loadeddata event has been fired, set the
        // element's delaying-the-load-event flag to false. This stops delaying the load event.
        m_delaying_the_load_event.clear();

        // If the new ready state is HAVE_FUTURE_DATA or HAVE_ENOUGH_DATA, then the relevant steps below must then be run also.
        if (ready_state != ReadyState::HaveFutureData && ready_state != ReadyState::HaveEnoughData)
            return;
    }

    // -> If the previous ready state was HAVE_FUTURE_DATA or more, and the new ready state is HAVE_CURRENT_DATA or less
    if (m_ready_state >= ReadyState::HaveFutureData && ready_state <= ReadyState::HaveCurrentData) {
        // FIXME: If the media element was potentially playing before its readyState attribute changed to a value lower than HAVE_FUTURE_DATA, and the element
        //        has not ended playback, and playback has not stopped due to errors, paused for user interaction, or paused for in-band content, the user agent
        //        must queue a media element task given the media element to fire an event named timeupdate at the element, and queue a media element task given
        //        the media element to fire an event named waiting at the element.
        return;
    }

    // -> If the previous ready state was HAVE_CURRENT_DATA or less, and the new ready state is HAVE_FUTURE_DATA
    if (m_ready_state <= ReadyState::HaveCurrentData && ready_state == ReadyState::HaveFutureData) {
        // The user agent must queue a media element task given the media element to fire an event named canplay at the element.
        queue_a_media_element_task([this] {
            dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::canplay));
        });

        // If the element's paused attribute is false, the user agent must notify about playing for the element.
        if (!paused())
            notify_about_playing();

        return;
    }

    // -> If the new ready state is HAVE_ENOUGH_DATA
    if (ready_state == ReadyState::HaveEnoughData) {
        // If the previous ready state was HAVE_CURRENT_DATA or less, the user agent must queue a media element task given the media element to fire an event
        // named canplay at the element, and, if the element's paused attribute is false, notify about playing for the element.
        if (m_ready_state <= ReadyState::HaveCurrentData) {
            queue_a_media_element_task([this] {
                dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::canplay));
            });

            if (!paused())
                notify_about_playing();
        }

        // The user agent must queue a media element task given the media element to fire an event named canplaythrough at the element.
        queue_a_media_element_task([this] {
            dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::canplaythrough));
        });

        // If the element is not eligible for autoplay, then the user agent must abort these substeps.
        if (!is_eligible_for_autoplay())
            return;

        // The user agent may run the following substeps:
        {
            // Set the paused attribute to false.
            set_paused(false);

            // If the element's show poster flag is true, set it to false and run the time marches on steps.
            if (m_show_poster) {
                set_show_poster(false);
                time_marches_on();
            }

            // Queue a media element task given the element to fire an event named play at the element.
            queue_a_media_element_task([this]() {
                dispatch_event(DOM::Event::create(realm(), HTML::EventNames::play));
            });

            // Notify about playing for the element.
            notify_about_playing();
        }

        // FIXME: Alternatively, if the element is a video element, the user agent may start observing whether the element intersects the viewport. When the
        //        element starts intersecting the viewport, if the element is still eligible for autoplay, run the substeps above. Optionally, when the element
        //        stops intersecting the viewport, if the can autoplay flag is still true and the autoplay attribute is still specified, run the following substeps:
        //            Run the internal pause steps and set the can autoplay flag to true.
        //            Queue a media element task given the element to fire an event named pause at the element.
        return;
    }
}

// https://html.spec.whatwg.org/multipage/media.html#internal-play-steps
WebIDL::ExceptionOr<void> HTMLMediaElement::play_element()
{
    // 1. If the media element's networkState attribute has the value NETWORK_EMPTY, invoke the media element's resource
    //    selection algorithm.
    if (m_network_state == NetworkState::Empty)
        TRY(select_resource());

    // 2. If the playback has ended and the direction of playback is forwards, seek to the earliest possible position
    //    of the media resource.
    if (has_ended_playback()) {
        // FIXME: Detect playback direction.
        seek_element(0);
    }

    // 3. If the media element's paused attribute is true, then:
    if (paused()) {
        // 1. Change the value of paused to false.
        set_paused(false);

        // 2. If the show poster flag is true, set the element's show poster flag to false and run the time marches on steps.
        if (m_show_poster) {
            set_show_poster(false);
            time_marches_on();
        }

        // 3. Queue a media element task given the media element to fire an event named play at the element.
        queue_a_media_element_task([this]() {
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::play));
        });

        // 4. If the media element's readyState attribute has the value HAVE_NOTHING, HAVE_METADATA, or HAVE_CURRENT_DATA,
        //    queue a media element task given the media element to fire an event named waiting at the element.
        if (m_ready_state == ReadyState::HaveNothing || m_ready_state == ReadyState::HaveMetadata || m_ready_state == ReadyState::HaveCurrentData) {
            queue_a_media_element_task([this]() {
                dispatch_event(DOM::Event::create(realm(), HTML::EventNames::waiting));
            });
        }
        //    Otherwise, the media element's readyState attribute has the value HAVE_FUTURE_DATA or HAVE_ENOUGH_DATA:
        //    notify about playing for the element.
        else {
            notify_about_playing();
        }
    }

    // 4. Otherwise, if the media element's readyState attribute has the value HAVE_FUTURE_DATA or HAVE_ENOUGH_DATA, take
    //    pending play promises and queue a media element task given the media element to resolve pending play promises
    //    with the result.
    else if (m_ready_state == ReadyState::HaveFutureData || m_ready_state == ReadyState::HaveEnoughData) {
        auto promises = take_pending_play_promises();

        queue_a_media_element_task([this, promises = move(promises)]() {
            resolve_pending_play_promises(promises);
        });
    }

    // 5. Set the media element's can autoplay flag to false.
    m_can_autoplay = false;

    return {};
}

// https://html.spec.whatwg.org/multipage/media.html#internal-pause-steps
WebIDL::ExceptionOr<void> HTMLMediaElement::pause_element()
{
    // 1. Set the media element's can autoplay flag to false.
    m_can_autoplay = false;

    // 2. If the media element's paused attribute is false, run the following steps:
    if (!paused()) {
        // 1. Change the value of paused to true.
        set_paused(true);

        // 2. Take pending play promises and let promises be the result.
        auto promises = take_pending_play_promises();

        // 3. Queue a media element task given the media element and the following steps:
        queue_a_media_element_task([this, promises = move(promises)]() {
            auto& realm = this->realm();

            // 1. Fire an event named timeupdate at the element.
            dispatch_time_update_event();

            // 2. Fire an event named pause at the element.
            dispatch_event(DOM::Event::create(realm, HTML::EventNames::pause));

            // 3. Reject pending play promises with promises and an "AbortError" DOMException.
            reject_pending_play_promises<WebIDL::AbortError>(promises, "Media playback was paused"_string);
        });

        // 4. Set the official playback position to the current playback position.
        m_official_playback_position = m_current_playback_position;
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/media.html#dom-media-seek
void HTMLMediaElement::seek_element(double playback_position, MediaSeekMode seek_mode)
{
    // 1. Set the media element's show poster flag to false.
    set_show_poster(false);

    // 2. If the media element's readyState is HAVE_NOTHING, return.
    if (m_ready_state == ReadyState::HaveNothing)
        return;

    // FIXME: 3. If the element's seeking IDL attribute is true, then another instance of this algorithm is already running.
    //           Abort that other instance of the algorithm without waiting for the step that it is running to complete.
    if (m_seeking) {
    }

    // 4. Set the seeking IDL attribute to true.
    set_seeking(true);

    // FIXME: 5. If the seek was in response to a DOM method call or setting of an IDL attribute, then continue the script. The
    //           remainder of these steps must be run in parallel. With the exception of the steps marked with ⌛, they could be
    //           aborted at any time by another instance of this algorithm being invoked.

    // 6. If the new playback position is later than the end of the media resource, then let it be the end of the media resource instead.
    if (playback_position > m_duration)
        playback_position = m_duration;

    // 7. If the new playback position is less than the earliest possible position, let it be that position instead.
    if (playback_position < 0)
        playback_position = 0;

    // FIXME: 8. If the (possibly now changed) new playback position is not in one of the ranges given in the seekable attribute,
    //           then let it be the position in one of the ranges given in the seekable attribute that is the nearest to the new
    //           playback position. If two positions both satisfy that constraint (i.e. the new playback position is exactly in the
    //           middle between two ranges in the seekable attribute) then use the position that is closest to the current playback
    //           position. If there are no ranges given in the seekable attribute then set the seeking IDL attribute to false and return.

    // 9. If the approximate-for-speed flag is set, adjust the new playback position to a value that will allow for playback to resume
    //    promptly. If new playback position before this step is before current playback position, then the adjusted new playback position
    //    must also be before the current playback position. Similarly, if the new playback position before this step is after current
    //    playback position, then the adjusted new playback position must also be after the current playback position.
    // NOTE: LibVideo handles approximation for speed internally.

    // 10. Queue a media element task given the media element to fire an event named seeking at the element.
    queue_a_media_element_task([this]() {
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::seeking));
    });

    // 11. Set the current playback position to the new playback position.
    set_current_playback_position(playback_position);

    // 12. Wait until the user agent has established whether or not the media data for the new playback position is
    //     available, and, if it is, until it has decoded enough data to play back that position.
    m_seek_in_progress = true;
    on_seek(playback_position, seek_mode);
    HTML::main_thread_event_loop().spin_until(GC::create_function(heap(), [&]() { return !m_seek_in_progress; }));

    // FIXME: 13. Await a stable state. The synchronous section consists of all the remaining steps of this algorithm. (Steps in the
    //            synchronous section are marked with ⌛.)

    // 14. ⌛ Set the seeking IDL attribute to false.
    set_seeking(false);

    // 15. ⌛ Run the time marches on steps.
    time_marches_on(TimeMarchesOnReason::Other);

    // 16. ⌛ Queue a media element task given the media element to fire an event named timeupdate at the element.
    queue_a_media_element_task([this]() {
        dispatch_time_update_event();
    });

    // 17. ⌛ Queue a media element task given the media element to fire an event named seeked at the element.
    queue_a_media_element_task([this]() {
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::seeked));
    });
}

// https://html.spec.whatwg.org/multipage/media.html#notify-about-playing
void HTMLMediaElement::notify_about_playing()
{
    // 1. Take pending play promises and let promises be the result.
    auto promises = take_pending_play_promises();

    // 2. Queue a media element task given the element and the following steps:
    queue_a_media_element_task([this, promises = move(promises)]() {
        // 1. Fire an event named playing at the element.
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::playing));

        // 2. Resolve pending play promises with promises.
        resolve_pending_play_promises(promises);
    });

    on_playing();

    if (m_audio_tracks->has_enabled_track())
        document().page().client().page_did_change_audio_play_state(AudioPlayState::Playing);
}

void HTMLMediaElement::set_show_poster(bool show_poster)
{
    if (m_show_poster == show_poster)
        return;

    m_show_poster = show_poster;

    if (auto* paintable = this->paintable())
        paintable->set_needs_display();
}

void HTMLMediaElement::set_paused(bool paused)
{
    if (m_paused == paused)
        return;

    m_paused = paused;

    if (m_paused) {
        on_paused();

        if (m_audio_tracks->has_enabled_track())
            document().page().client().page_did_change_audio_play_state(AudioPlayState::Paused);
    }

    if (auto* paintable = this->paintable())
        paintable->set_needs_display();
    set_needs_style_update(true);
}

// https://html.spec.whatwg.org/multipage/media.html#blocked-media-element
bool HTMLMediaElement::blocked() const
{
    // A media element is a blocked media element if its readyState attribute is in the HAVE_NOTHING state, the HAVE_METADATA
    // state, or the HAVE_CURRENT_DATA state, or if the element has paused for user interaction or paused for in-band content.
    switch (m_ready_state) {
    case ReadyState::HaveNothing:
    case ReadyState::HaveMetadata:
    case ReadyState::HaveCurrentData:
        return true;
    default:
        break;
    }

    // FIXME: Implement "paused for user interaction" (namely "the user agent has reached a point in the media resource
    //        where the user has to make a selection for the resource to continue").
    // FIXME: Implement "paused for in-band content".
    return false;
}

bool HTMLMediaElement::stalled() const
{
    // FIXME: Implement stall timeout. https://html.spec.whatwg.org/multipage/media.html#stall-timeout
    return false;
}

// https://html.spec.whatwg.org/multipage/media.html#potentially-playing
bool HTMLMediaElement::potentially_playing() const
{
    // A media element is said to be potentially playing when its paused attribute is false, the element has not ended
    // playback, playback has not stopped due to errors, and the element is not a blocked media element.
    // FIXME: Implement "stopped due to errors".
    return !paused() && !ended() && !blocked();
}

// https://html.spec.whatwg.org/multipage/media.html#eligible-for-autoplay
bool HTMLMediaElement::is_eligible_for_autoplay() const
{
    // A media element is said to be eligible for autoplay when all of the following conditions are met:
    return (
        // Its can autoplay flag is true.
        m_can_autoplay &&

        // Its paused attribute is true.
        paused() &&

        // It has an autoplay attribute specified.
        has_attribute(HTML::AttributeNames::autoplay) &&

        // Its node document's active sandboxing flag set does not have the sandboxed automatic features browsing context flag set.
        !has_flag(document().active_sandboxing_flag_set(), SandboxingFlagSet::SandboxedAutomaticFeatures) &&

        // Its node document is allowed to use the "autoplay" feature.
        document().is_allowed_to_use_feature(DOM::PolicyControlledFeature::Autoplay));
}

// https://html.spec.whatwg.org/multipage/media.html#ended-playback
bool HTMLMediaElement::has_ended_playback() const
{
    // A media element is said to have ended playback when:

    // The element's readyState attribute is HAVE_METADATA or greater, and
    if (m_ready_state < ReadyState::HaveMetadata)
        return false;

    // Either:
    if (
        // The current playback position is the end of the media resource, and
        m_current_playback_position == m_duration &&

        // FIXME: The direction of playback is forwards, and

        // The media element does not have a loop attribute specified.
        !has_attribute(HTML::AttributeNames::loop)) {
        return true;
    }

    // FIXME: Or:
    //            The current playback position is the earliest possible position, and
    //            The direction of playback is backwards.

    return false;
}

// https://html.spec.whatwg.org/multipage/media.html#reaches-the-end
void HTMLMediaElement::reached_end_of_media_playback()
{
    // 1. If the media element has a loop attribute specified, then seek to the earliest possible position of the media resource and return.
    if (has_attribute(HTML::AttributeNames::loop)) {
        seek_element(0);

        // AD-HOC: LibVideo internally sets itself to a paused state when it reaches the end of a video. We must resume
        //         playing manually to actually loop. Note that we don't need to update any HTMLMediaElement state as
        //         it hasn't left the playing state by this point.
        on_playing();
    }

    // 2. As defined above, the ended IDL attribute starts returning true once the event loop returns to step 1.

    // 3. Queue a media element task given the media element and the following steps:
    queue_a_media_element_task([this]() mutable {
        // 1. Fire an event named timeupdate at the media element.
        dispatch_time_update_event();

        // 2. If the media element has ended playback, the direction of playback is forwards, and paused is false, then:
        // FIXME: Detect playback direction.
        if (has_ended_playback() && !paused()) {
            // 1. Set the paused attribute to true.
            set_paused(true);

            // 2. Fire an event named pause at the media element.
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::pause));

            // 3. Take pending play promises and reject pending play promises with the result and an "AbortError" DOMException.
            auto promises = take_pending_play_promises();
            reject_pending_play_promises<WebIDL::AbortError>(promises, "Media playback has ended"_string);
        }
    });

    // 4. Fire an event named ended at the media element.
    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::ended));
}

void HTMLMediaElement::dispatch_time_update_event()
{
    ScopeGuard guard { [this] { m_running_time_update_event_handler = false; } };
    m_running_time_update_event_handler = true;

    m_last_time_update_event_time = MonotonicTime::now();

    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::timeupdate));
}

// https://html.spec.whatwg.org/multipage/media.html#time-marches-on
void HTMLMediaElement::time_marches_on(TimeMarchesOnReason reason)
{
    // FIXME: 1. Let current cues be a list of cues, initialized to contain all the cues of all the hidden or showing text tracks
    //           of the media element (not the disabled ones) whose start times are less than or equal to the current playback
    //           position and whose end times are greater than the current playback position.
    // FIXME: 2. Let other cues be a list of cues, initialized to contain all the cues of hidden and showing text tracks of the
    //           media element that are not present in current cues.
    // FIXME: 3. Let last time be the current playback position at the time this algorithm was last run for this media element,
    //           if this is not the first time it has run.
    // FIXME: 4. If the current playback position has, since the last time this algorithm was run, only changed through its usual
    //           monotonic increase during normal playback, then let missed cues be the list of cues in other cues whose start times
    //           are greater than or equal to last time and whose end times are less than or equal to the current playback position.
    //           Otherwise, let missed cues be an empty list.
    // FIXME: 5. Remove all the cues in missed cues that are also in the media element's list of newly introduced cues, and then
    //           empty the element's list of newly introduced cues.

    // 6. If the time was reached through the usual monotonic increase of the current playback position during normal
    //    playback, and if the user agent has not fired a timeupdate event at the element in the past 15 to 250ms and is
    //    not still running event handlers for such an event, then the user agent must queue a media element task given
    //    the media element to fire an event named timeupdate at the element. (In the other cases, such as explicit seeks,
    //    relevant events get fired as part of the overall process of changing the current playback position.)
    if (reason == TimeMarchesOnReason::NormalPlayback && !m_running_time_update_event_handler) {
        auto dispatch_event = true;

        if (m_last_time_update_event_time.has_value()) {
            auto time_since_last_event = MonotonicTime::now() - *m_last_time_update_event_time;
            dispatch_event = time_since_last_event.to_milliseconds() > 250;
        }

        if (dispatch_event) {
            queue_a_media_element_task([this]() {
                dispatch_time_update_event();
            });
        }
    }

    // FIXME: 7. If all of the cues in current cues have their text track cue active flag set, none of the cues in other cues have
    //           their text track cue active flag set, and missed cues is empty, then return.
    // FIXME: 8. If the time was reached through the usual monotonic increase of the current playback position during normal playback,
    //           and there are cues in other cues that have their text track cue pause-on-exit flag set and that either have their
    //           text track cue active flag set or are also in missed cues, then immediately pause the media element.
    // FIXME: 9. Let events be a list of tasks, initially empty. Each task in this list will be associated with a text track, a
    //           text track cue, and a time, which are used to sort the list before the tasks are queued.
    //
    //           Let affected tracks be a list of text tracks, initially empty.
    //
    //           When the steps below say to prepare an event named event for a text track cue target with a time time, the user
    //           agent must run these steps:
    //               1. Let track be the text track with which the text track cue target is associated.
    //               2. Create a task to fire an event named event at target.
    //               3. Add the newly created task to events, associated with the time time, the text track track, and the text
    //                  track cue target.
    //               4. Add track to affected tracks.
    // FIXME: 10. For each text track cue in missed cues, prepare an event named enter for the TextTrackCue object with the text
    //            track cue start time.
    // FIXME: 11. For each text track cue in other cues that either has its text track cue active flag set or is in missed cues,
    //            prepare an event named exit for the TextTrackCue object with the later of the text track cue end time and the
    ///           text track cue start time.
    // FIXME: 12. For each text track cue in current cues that does not have its text track cue active flag set, prepare an event
    //            named enter for the TextTrackCue object with the text track cue start time.
    // FIXME: 13. Sort the tasks in events in ascending time order (tasks with earlier times first).
    //
    //            Further sort tasks in events that have the same time by the relative text track cue order of the text track cues
    //            associated with these tasks.
    //
    //            Finally, sort tasks in events that have the same time and same text track cue order by placing tasks that fire
    //            enter events before those that fire exit events.
    // FIXME: 14. Queue a media element task given the media element for each task in events, in list order.
    // FIXME: 15. Sort affected tracks in the same order as the text tracks appear in the media element's list of text tracks, and
    //            remove duplicates.
    // FIXME: 16. For each text track in affected tracks, in the list order, queue a media element task given the media element to
    //            fire an event named cuechange at the TextTrack object, and, if the text track has a corresponding track element,
    //            to then fire an event named cuechange at the track element as well.
    // FIXME: 17. Set the text track cue active flag of all the cues in the current cues, and unset the text track cue active flag
    //            of all the cues in the other cues.
    // FIXME: 18. Run the rules for updating the text track rendering of each of the text tracks in affected tracks that are showing,
    //            providing the text track's text track language as the fallback language if it is not the empty string. For example,
    //            for text tracks based on WebVTT, the rules for updating the display of WebVTT text tracks.
}

// https://html.spec.whatwg.org/multipage/media.html#take-pending-play-promises
GC::RootVector<GC::Ref<WebIDL::Promise>> HTMLMediaElement::take_pending_play_promises()
{
    // 1. Let promises be an empty list of promises.
    // 2. Copy the media element's list of pending play promises to promises.
    // 3. Clear the media element's list of pending play promises.
    GC::RootVector<GC::Ref<WebIDL::Promise>> promises(heap());
    promises.extend(move(m_pending_play_promises));

    // 4. Return promises.
    return promises;
}

// https://html.spec.whatwg.org/multipage/media.html#resolve-pending-play-promises
void HTMLMediaElement::resolve_pending_play_promises(ReadonlySpan<GC::Ref<WebIDL::Promise>> promises)
{
    auto& realm = this->realm();

    // AD-HOC: An execution context is required for Promise resolving hooks.
    TemporaryExecutionContext execution_context { realm };

    // To resolve pending play promises for a media element with a list of promises promises, the user agent
    // must resolve each promise in promises with undefined.
    for (auto const& promise : promises)
        WebIDL::resolve_promise(realm, promise, JS::js_undefined());
}

// https://html.spec.whatwg.org/multipage/media.html#reject-pending-play-promises
void HTMLMediaElement::reject_pending_play_promises(ReadonlySpan<GC::Ref<WebIDL::Promise>> promises, GC::Ref<WebIDL::DOMException> error)
{
    auto& realm = this->realm();

    // AD-HOC: An execution context is required for Promise rejection hooks.
    TemporaryExecutionContext execution_context { realm };

    // To reject pending play promises for a media element with a list of promise promises and an exception name
    // error, the user agent must reject each promise in promises with error.
    for (auto const& promise : promises)
        WebIDL::reject_promise(realm, promise, error);
}

WebIDL::ExceptionOr<bool> HTMLMediaElement::handle_keydown(Badge<Web::EventHandler>, UIEvents::KeyCode key, u32 modifiers)
{
    if (modifiers != UIEvents::KeyModifier::Mod_None)
        return false;

    switch (key) {
    case UIEvents::KeyCode::Key_Space:
        TRY(toggle_playback());
        break;

    case UIEvents::KeyCode::Key_Home:
        set_current_time(0);
        break;
    case UIEvents::KeyCode::Key_End:
        set_current_time(duration());
        break;

    case UIEvents::KeyCode::Key_Left:
    case UIEvents::KeyCode::Key_Right: {
        static constexpr double time_skipped_per_key_press = 5.0;
        auto current_time = this->current_time();

        if (key == UIEvents::KeyCode::Key_Left)
            current_time = max(0.0, current_time - time_skipped_per_key_press);
        else
            current_time = min(duration(), current_time + time_skipped_per_key_press);

        set_current_time(current_time);
        break;
    }

    case UIEvents::KeyCode::Key_Up:
    case UIEvents::KeyCode::Key_Down: {
        static constexpr double volume_change_per_key_press = 0.1;
        auto volume = this->volume();

        if (key == UIEvents::KeyCode::Key_Up)
            volume = min(1.0, volume + volume_change_per_key_press);
        else
            volume = max(0.0, volume - volume_change_per_key_press);

        TRY(set_volume(volume));
        break;
    }

    case UIEvents::KeyCode::Key_M:
        set_muted(!muted());
        break;

    default:
        return false;
    }

    return true;
}

void HTMLMediaElement::set_layout_display_time(Badge<Painting::MediaPaintable>, Optional<double> display_time)
{
    if (display_time.has_value() && !m_display_time.has_value()) {
        if (potentially_playing()) {
            m_tracking_mouse_position_while_playing = true;
            on_paused();
        }
    } else if (!display_time.has_value() && m_display_time.has_value()) {
        if (m_tracking_mouse_position_while_playing) {
            m_tracking_mouse_position_while_playing = false;
            on_playing();
        }
    }

    m_display_time = move(display_time);

    if (auto* paintable = this->paintable())
        paintable->set_needs_display();
}

double HTMLMediaElement::layout_display_time(Badge<Painting::MediaPaintable>) const
{
    return m_display_time.value_or(current_time());
}

}

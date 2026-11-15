/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaSession/ActiveMediaSessionTracker.h>
#include <LibWeb/MediaSession/MediaMetadata.h>
#include <LibWeb/MediaSession/MediaSession.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
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
    update_active_document();
    update_position_states();
    if (!m_active_document.has_value())
        return {};

    auto* document = as_if<DOM::Document>(DOM::Node::from_unique_id(*m_active_document));
    if (!document || !document->is_fully_active() || document->has_been_destroyed())
        return {};

    auto session = document->window()->navigator()->media_session();
    MediaSessionState state;
    state.activation_counter = m_activation_counter;

    // https://w3c.github.io/mediasession/#update-metadata-algorithm
    // 2. If the metadata of the active media session is an empty metadata, unset the media metadata presented to the
    //    platform, and terminate these steps.
    // 3. Update the media metadata presented to the platform to match the metadata for the active media session.
    // NB: Empty strings represent unset metadata. Artwork isn't part of this state.
    if (auto metadata = session->metadata()) {
        state.title = MUST(metadata->title().utf16_view().to_utf8());
        state.artist = MUST(metadata->artist().utf16_view().to_utf8());
        state.album = MUST(metadata->album().utf16_view().to_utf8());
    }

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
    if (states_match(m_last_reported_state, state))
        return;
    m_last_reported_state = state;
    m_page->client().page_did_change_media_session_state(state);
}

}

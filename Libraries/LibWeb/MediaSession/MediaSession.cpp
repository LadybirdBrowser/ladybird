/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/MediaSession/MediaMetadata.h>
#include <LibWeb/MediaSession/MediaSession.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <math.h>

namespace Web::MediaSession {

GC_DEFINE_ALLOCATOR(MediaSession);

GC::Ref<MediaSession> MediaSession::create()
{
    return GC::Heap::the().allocate<MediaSession>();
}

void MediaSession::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_metadata);
    visitor.visit(m_action_handlers);
}

// https://w3c.github.io/mediasession/#dom-mediasession-metadata
void MediaSession::set_metadata(GC::Ptr<MediaMetadata> value)
{
    // 1. If the MediaSession's metadata is not null, set its media session to null.
    if (m_metadata)
        m_metadata->set_media_session(nullptr);

    // 2. Set the MediaSession's metadata to value.
    m_metadata = value;

    // 3. If the MediaSession's metadata is not null, set its media session to the current MediaSession.
    if (m_metadata)
        m_metadata->set_media_session(this);

    // FIXME: 4. In parallel, run the update metadata algorithm.
}

// https://w3c.github.io/mediasession/#dom-mediasession-playbackstate
void MediaSession::set_playback_state(Bindings::MediaSessionPlaybackState value)
{
    // On setting, the user agent MUST set the IDL attribute to the new value if it is a valid
    // MediaSessionPlaybackState value.
    m_playback_state = value;

    // FIXME: Setting playbackState may cause the actual playback state to change and run the media session actions
    //        update algorithm.
}

// https://w3c.github.io/mediasession/#update-action-handler-algorithm
void MediaSession::set_action_handler(Bindings::MediaSessionAction action, GC::Ptr<WebIDL::CallbackType> handler)
{
    // 1. If handler is null, remove action from the supported media session actions for MediaSession and abort these steps.
    if (!handler) {
        m_action_handlers.remove(action);
        // FIXME: Run the media session actions update algorithm when the supported media session actions change.
        return;
    }

    // 2. Add action to the supported media session actions for MediaSession and associate to it the handler.
    m_action_handlers.set(action, handler);

    // FIXME: Run the media session actions update algorithm when the supported media session actions change.
}

// https://w3c.github.io/mediasession/#dom-mediasession-setpositionstate
WebIDL::ExceptionOr<void> MediaSession::set_position_state(Bindings::MediaPositionState state)
{
    // 1. If state is an empty dictionary, clear the position state and abort these steps.
    if (!state.duration.has_value() && !state.position.has_value() && !state.playback_rate.has_value()) {
        m_position_state.clear();
        return {};
    }

    // 2. If state's duration is not present, throw a TypeError.
    if (!state.duration.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Position state requires a duration"_utf16 };

    // 3. If state's duration is negative or NaN, throw a TypeError.
    if (*state.duration < 0 || isnan(*state.duration))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Position state duration must be non-negative and not NaN"_utf16 };

    // 4. If state's position is not present, set it to zero.
    if (!state.position.has_value())
        state.position = 0;

    // 5. If state's position is negative or greater than duration, throw a TypeError.
    if (*state.position < 0 || *state.position > *state.duration)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Position must be between zero and duration"_utf16 };

    // 6. If state's playbackRate is not present, set it to 1.0.
    if (!state.playback_rate.has_value())
        state.playback_rate = 1.0;

    // 7. If state's playbackRate is zero, throw a TypeError.
    if (*state.playback_rate == 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Position state playback rate must be nonzero"_utf16 };

    // 8. Update the position state and last position updated time.
    m_position_state = PositionState { *state.duration, *state.playback_rate, *state.position, MonotonicTime::now() };
    return {};
}

}

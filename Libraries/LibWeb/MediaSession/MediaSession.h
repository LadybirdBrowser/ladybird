/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibWeb/Bindings/MediaSession.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/MediaSession/MediaSessionState.h>

namespace Web::MediaSession {

// https://w3c.github.io/mediasession/#mediasession
class MediaSession final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(MediaSession, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(MediaSession);

public:
    static GC::Ref<MediaSession> create(HTML::Window&);
    virtual ~MediaSession() override = default;

    GC::Ptr<MediaMetadata> metadata() const { return m_metadata; }
    void set_metadata(GC::Ptr<MediaMetadata>);

    Bindings::MediaSessionPlaybackState playback_state() const { return m_playback_state; }
    void set_playback_state(Bindings::MediaSessionPlaybackState);

    void set_action_handler(Bindings::MediaSessionAction, GC::Ptr<WebIDL::CallbackType>);
    WebIDL::ExceptionOr<void> set_position_state(Bindings::MediaPositionState);

    Optional<MediaSessionPositionState> const& position_state() const { return m_position_state; }
    auto const& action_handlers() const { return m_action_handlers; }
    void state_changed();

private:
    explicit MediaSession(HTML::Window&);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<HTML::Window> m_window;
    GC::Ptr<MediaMetadata> m_metadata;
    Bindings::MediaSessionPlaybackState m_playback_state { Bindings::MediaSessionPlaybackState::None };
    HashMap<Bindings::MediaSessionAction, GC::Ptr<WebIDL::CallbackType>> m_action_handlers;

    Optional<MediaSessionPositionState> m_position_state;
};

}

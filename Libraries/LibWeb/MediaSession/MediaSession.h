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

namespace Web::MediaSession {

// https://w3c.github.io/mediasession/#mediasession
class MediaSession final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(MediaSession, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(MediaSession);

public:
    static GC::Ref<MediaSession> create();
    virtual ~MediaSession() override = default;

    GC::Ptr<MediaMetadata> metadata() const { return m_metadata; }
    void set_metadata(GC::Ptr<MediaMetadata>);

    Bindings::MediaSessionPlaybackState playback_state() const { return m_playback_state; }
    void set_playback_state(Bindings::MediaSessionPlaybackState);

    void set_action_handler(Bindings::MediaSessionAction, GC::Ptr<WebIDL::CallbackType>);
    WebIDL::ExceptionOr<void> set_position_state(Bindings::MediaPositionState);

private:
    MediaSession() = default;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<MediaMetadata> m_metadata;
    Bindings::MediaSessionPlaybackState m_playback_state { Bindings::MediaSessionPlaybackState::None };
    HashMap<Bindings::MediaSessionAction, GC::Ptr<WebIDL::CallbackType>> m_action_handlers;

    struct PositionState {
        double duration;
        double playback_rate;
        double position;
        MonotonicTime last_position_updated_time;
    };
    Optional<PositionState> m_position_state;
};

}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/kmalloc.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>
#include <LibWeb/MediaSession/MediaSessionState.h>
#include <LibWebCommon/Forward.h>

namespace Web::MediaSession {

class ActiveMediaSessionTracker {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ActiveMediaSessionTracker(Page&);

    void media_element_became_audible(HTML::HTMLMediaElement&);
    void media_element_removed(HTML::HTMLMediaElement&);
    void media_element_unregistered(UniqueNodeID);
    void document_became_inactive(DOM::Document&);
    void mark_dirty();

    Optional<MediaSessionState> compute_state();

private:
    void update_active_document();
    void update_position_states();
    Bindings::MediaSessionPlaybackState actual_playback_state(DOM::Document&);
    void report_state();

    struct ControlledElement {
        UniqueNodeID element_id;
        UniqueNodeID document_id;
        u64 activation_order;
        bool audible_and_controllable;
    };

    struct PositionState {
        Optional<MediaSessionPositionState> explicit_position;
        Optional<MediaSessionPositionState> reported_position;
        Bindings::MediaSessionPlaybackState playback_state { Bindings::MediaSessionPlaybackState::Paused };
    };

    GC::Ref<Page> m_page;
    GC::Weak<Page> m_weak_page;
    Optional<UniqueNodeID> m_active_document;
    Vector<ControlledElement> m_controlled_elements;
    u64 m_activation_counter { 0 };
    HashMap<UniqueNodeID, PositionState> m_position_states;
    Optional<MediaSessionState> m_last_reported_state;
    bool m_report_scheduled { false };
};

}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/kmalloc.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/MediaSession/MediaSessionState.h>

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
    void visit_edges(GC::Cell::Visitor&);
    void when_artwork_settled(GC::Ref<WebIDL::Promise>);

    Optional<MediaSessionState> compute_state();
    GC::Ptr<DOM::Document> active_document();
    void handle_action(Bindings::MediaSessionAction, Optional<double> seek_offset, Optional<double> seek_time, Optional<bool> fast_seek);

private:
    void update_active_document();
    void update_position_states();
    Bindings::MediaSessionPlaybackState actual_playback_state(DOM::Document&);
    void report_state();
    void update_artwork();
    bool artwork_document_is_active();
    void fetch_next_artwork();
    void finish_artwork(RefPtr<Gfx::Bitmap const>);
    void abandon_artwork_fetch();
    void run_default_action(Bindings::MediaSessionAction, Optional<double> seek_offset, Optional<double> seek_time, Optional<bool> fast_seek);

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
    Vector<UniqueNodeID> m_paused_by_user_agent;
    Optional<MediaSessionState> m_last_reported_state;
    bool m_report_scheduled { false };

    Optional<UniqueNodeID> m_artwork_document;
    GC::Ptr<MediaMetadata> m_artwork_metadata;
    u64 m_artwork_version { 0 };
    Vector<URL::URL> m_artwork_urls;
    size_t m_next_artwork_index { 0 };
    u64 m_artwork_generation { 0 };
    GC::Ptr<Fetch::Infrastructure::FetchController> m_artwork_fetch_controller;
    bool m_artwork_fetch_in_flight { false };
    RefPtr<Gfx::Bitmap const> m_artwork;
    Vector<GC::Ref<WebIDL::Promise>> m_artwork_waiters;
};

}

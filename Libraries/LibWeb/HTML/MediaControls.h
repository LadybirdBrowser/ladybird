/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/Timer.h>
#include <LibGC/Cell.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/MediaControlsDOM.h>

namespace Web::HTML {

class HTMLMediaElement;

class MediaControls {
public:
    explicit MediaControls(HTMLMediaElement&);
    ~MediaControls();

    void visit_edges(GC::Cell::Visitor&);

private:
    void create_shadow_tree();

    enum class ListenOnce : bool {
        No,
        Yes,
    };

    template<typename T, CallableAs<bool, T&> Handler>
    GC::Ref<DOM::IDLEventListener> add_event_listener(JS::Realm&, DOM::EventTarget&, FlyString const& event_name, ListenOnce, Handler);
    template<CallableAs<bool> Handler>
    GC::Ref<DOM::IDLEventListener> add_event_listener(JS::Realm&, DOM::EventTarget&, FlyString const& event_nam, Handler);
    template<CallableAs<bool, UIEvents::MouseEvent const&> Handler>
    GC::Ref<DOM::IDLEventListener> add_event_listener(JS::Realm&, DOM::EventTarget&, FlyString const& event_name, Handler);
    template<CallableAs<bool, UIEvents::MouseEvent const&> Handler>
    GC::Ref<DOM::IDLEventListener> add_event_listener(JS::Realm&, DOM::EventTarget&, FlyString const& event_name, ListenOnce, Handler);
    template<CallableAs<bool, UIEvents::KeyboardEvent const&> Handler>
    GC::Ref<DOM::IDLEventListener> add_event_listener(JS::Realm&, DOM::EventTarget&, FlyString const& event_name, Handler);

    void remove_event_listeners();
    void set_up_event_listeners();

    void play();
    void toggle_playback();
    void set_current_time(double);
    void set_volume(double);
    void toggle_mute();
    void toggle_fullscreen();

    void update_play_pause_icon();
    void update_timeline();
    void set_timeline_progress(double);
    void update_timestamp();
    void set_timestamp(double time, double duration);
    void request_timeline_update();
    void update_volume_and_mute_indicator();
    void update_fullscreen_icon();
    void update_placeholder_visibility();

    bool should_show_placeholder() const;

    void show_controls();
    void hide_controls();

    GC::Weak<HTMLMediaElement> m_media_element;

    Optional<MediaControlsDOM> m_dom;

    struct RegisteredEventListener {
        GC::Weak<DOM::EventTarget> target;
        FlyString event_name;
        GC::Weak<DOM::IDLEventListener> listener;
    };
    Vector<RegisteredEventListener> m_registered_event_listeners;
    GC::Ptr<WebIDL::CallbackType> m_request_animation_frame_callback;
    u32 m_request_animation_frame_id { 0 };

    enum class Scrubbing : u8 {
        No,
        WhilePaused,
        WhilePlaying,
    };
    Scrubbing m_scrubbing_timeline { Scrubbing::No };
    bool m_scrubbing_volume { false };
    bool m_hovering_controls { false };

    RefPtr<Core::Timer> m_hover_timer;

    bool m_had_audio { true };
    bool m_was_muted { false };
    enum class MuteIconState : u8 {
        Empty,
        Low,
        High,
    };
    MuteIconState m_mute_icon_state { MuteIconState::Empty };

    double m_last_timeline_progress { 0.0 };
    i64 m_last_timestamp_time { -1 };
    i64 m_last_timestamp_duration { -1 };

    struct BufferedRange {
        GC::Weak<DOM::Element> element;
        double left { 0 };
        double width { 0 };
    };
    Vector<BufferedRange> m_buffered_ranges;
};

}

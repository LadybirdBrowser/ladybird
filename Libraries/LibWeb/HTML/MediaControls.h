/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Vector.h>
#include <LibCore/Timer.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class HTMLMediaElement;

class MediaControls {
public:
    explicit MediaControls(HTMLMediaElement&);
    ~MediaControls();

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

    void toggle_playback();
    void set_current_time(double);
    void set_volume(double);
    void toggle_mute();

    void update_play_pause_icon();
    void update_timeline();
    void update_timestamp();
    void update_volume_and_mute_indicator();
    void update_placeholder_visibility();

    void show_controls();
    void hide_controls();

    GC::Weak<HTMLMediaElement> m_media_element;

    GC::Weak<DOM::Element> m_control_bar;
    GC::Weak<DOM::Element> m_timeline_element;
    GC::Weak<DOM::Element> m_timeline_fill;
    GC::Weak<DOM::Element> m_play_button;
    GC::Weak<DOM::Element> m_play_pause_icon;
    GC::Weak<DOM::Element> m_timestamp_element;
    GC::Weak<DOM::Element> m_mute_button;
    GC::Weak<DOM::Element> m_volume_area;
    GC::Weak<DOM::Element> m_volume_element;
    GC::Weak<DOM::Element> m_volume_fill;
    GC::Weak<DOM::Element> m_video_overlay;
    GC::Weak<DOM::Element> m_placeholder_circle;

    struct RegisteredEventListener {
        GC::Weak<DOM::EventTarget> target;
        FlyString event_name;
        GC::Weak<DOM::IDLEventListener> listener;
    };
    Vector<RegisteredEventListener> m_registered_event_listeners;

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
};

}

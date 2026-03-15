/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumberFormat.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/MediaControls.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::HTML {

MediaControls::MediaControls(HTMLMediaElement& media_element)
    : m_media_element(media_element)
{
    create_shadow_tree();
    set_up_event_listeners();
}

MediaControls::~MediaControls()
{
    remove_event_listeners();
    if (m_media_element)
        m_media_element->set_shadow_root(nullptr);
}

void MediaControls::create_shadow_tree()
{
    auto& media_element = *m_media_element;
    auto& document = media_element.document();
    auto& realm = media_element.realm();

    bool is_video = is<HTMLVideoElement>(media_element);

    auto shadow_root = realm.create<DOM::ShadowRoot>(document, media_element, Bindings::ShadowRootMode::Closed);
    shadow_root->set_user_agent_internal(true);
    media_element.set_shadow_root(shadow_root);

    m_dom = MediaControlsDOM(document, *shadow_root, is_video ? MediaControlsDOM::Options::Video : MediaControlsDOM::Options::None);

    static Vector<String> s_video_class = { "video"_string };
    static Vector<String> s_audio_class = { "audio"_string };
    if (is_video)
        MUST(m_dom->container->class_list()->add(s_video_class));
    else
        MUST(m_dom->container->class_list()->add(s_audio_class));

    // Initialize state
    update_play_pause_icon();
    update_timestamp();
    update_volume_and_mute_indicator();
    update_fullscreen_icon();
    update_placeholder_visibility();

    show_controls();
}

template<typename T, CallableAs<bool, T&> Handler>
GC::Ref<DOM::IDLEventListener> MediaControls::add_event_listener(JS::Realm& realm, DOM::EventTarget& target, FlyString const& event_name, ListenOnce listen_once, Handler handler)
{
    auto callback_function = JS::NativeFunction::create(
        realm, [handler = move(handler)](JS::VM& vm) {
            if (auto event = vm.argument(0).as_if<T>()) {
                if (handler(*event))
                    event->prevent_default();
            }
            return JS::js_undefined();
        },
        0, Utf16FlyString {}, &realm);
    auto callback = realm.heap().allocate<WebIDL::CallbackType>(*callback_function, realm);
    auto listener = DOM::IDLEventListener::create(realm, callback);

    DOM::AddEventListenerOptions options;
    options.once = listen_once == ListenOnce::Yes;
    target.add_event_listener(event_name, listener, options);

    m_registered_event_listeners.empend(target, event_name, listener);
    return listener;
}

template<CallableAs<bool> Handler>
GC::Ref<DOM::IDLEventListener> MediaControls::add_event_listener(JS::Realm& realm, DOM::EventTarget& target, FlyString const& event_name, Handler handler)
{
    return add_event_listener<DOM::Event>(realm, target, event_name, ListenOnce::No, [handler = move(handler)](DOM::Event&) {
        return handler();
    });
}

template<CallableAs<bool, UIEvents::MouseEvent const&> Handler>
GC::Ref<DOM::IDLEventListener> MediaControls::add_event_listener(JS::Realm& realm, DOM::EventTarget& target, FlyString const& event_name, Handler handler)
{
    return add_event_listener<UIEvents::MouseEvent>(realm, target, event_name, ListenOnce::No, handler);
}

template<CallableAs<bool, UIEvents::MouseEvent const&> Handler>
GC::Ref<DOM::IDLEventListener> MediaControls::add_event_listener(JS::Realm& realm, DOM::EventTarget& target, FlyString const& event_name, ListenOnce listen_once, Handler handler)
{
    return add_event_listener<UIEvents::MouseEvent>(realm, target, event_name, listen_once, handler);
}

template<CallableAs<bool, UIEvents::KeyboardEvent const&> Handler>
GC::Ref<DOM::IDLEventListener> MediaControls::add_event_listener(JS::Realm& realm, DOM::EventTarget& target, FlyString const& event_name, Handler handler)
{
    return add_event_listener<UIEvents::KeyboardEvent>(realm, target, event_name, ListenOnce::No, handler);
}

void MediaControls::remove_event_listeners()
{
    for (auto const& [target, event_name, listener] : m_registered_event_listeners) {
        if (!target)
            continue;
        if (!listener)
            continue;
        target->remove_event_listener_without_options(event_name, *listener);
    }
    m_registered_event_listeners.clear();
}

void MediaControls::set_up_event_listeners()
{
    auto& media_element = *m_media_element;
    auto& realm = media_element.realm();
    auto& window = as<HTML::Window>(realm.global_object());

    // Media element state events
    add_event_listener(realm, media_element, HTML::EventNames::play, [this]() {
        update_play_pause_icon();
        update_placeholder_visibility();
        return true;
    });
    add_event_listener(realm, media_element, HTML::EventNames::pause, [this] {
        update_play_pause_icon();
        update_placeholder_visibility();
        return true;
    });
    add_event_listener(realm, media_element, HTML::EventNames::playing, [this] {
        update_play_pause_icon();
        update_placeholder_visibility();
        return true;
    });
    add_event_listener(realm, media_element, HTML::EventNames::seeked, [this] {
        update_placeholder_visibility();
        return true;
    });
    add_event_listener(realm, media_element, HTML::EventNames::timeupdate, [this] {
        update_timeline();
        update_timestamp();
        return true;
    });
    add_event_listener(realm, media_element, HTML::EventNames::durationchange, [this] {
        update_timeline();
        update_timestamp();
        return true;
    });
    add_event_listener(realm, media_element, HTML::EventNames::volumechange, [this] {
        update_volume_and_mute_indicator();
        return true;
    });
    add_event_listener(realm, media_element, HTML::EventNames::loadedmetadata, [this] {
        update_timestamp();
        update_volume_and_mute_indicator();
        return true;
    });
    add_event_listener(realm, media_element, HTML::EventNames::addtrack, [this] {
        update_volume_and_mute_indicator();
        return true;
    });
    add_event_listener(realm, media_element, HTML::EventNames::emptied, [this] {
        update_placeholder_visibility();
        update_timeline();
        update_timestamp();
        return true;
    });

    if (m_dom->fullscreen_button) {
        add_event_listener(realm, media_element.document(), HTML::EventNames::fullscreenchange, [this] {
            update_fullscreen_icon();
            return true;
        });
    }

    // Play/pause button
    add_event_listener(realm, *m_dom->play_button, UIEvents::EventNames::click, [this] {
        toggle_playback();
        return true;
    });

    // Video overlay click — toggle playback when clicking outside the controls
    if (m_dom->video_overlay) {
        add_event_listener(realm, *m_dom->video_overlay, UIEvents::EventNames::click, [this] {
            toggle_playback();
            return true;
        });
    }

    // Timeline scrubbing
    static constexpr auto compute_timeline_position = [](UIEvents::MouseEvent const& event, DOM::Element& timeline_element, double duration) -> Optional<double> {
        if (isnan(duration) || duration == 0.0)
            return {};
        auto rect = timeline_element.get_bounding_client_rect();
        auto fraction = clamp((event.client_x() - rect.left().to_double()) / rect.width().to_double(), 0.0, 1.0);
        return fraction * duration;
    };

    add_event_listener(realm, *m_dom->timeline_element, UIEvents::EventNames::mousedown, [this](UIEvents::MouseEvent const& event) {
        VERIFY(m_media_element);
        VERIFY(m_dom->timeline_element);

        auto position = compute_timeline_position(event, *m_dom->timeline_element, m_media_element->duration());
        if (!position.has_value())
            return false;

        m_scrubbing_timeline = Scrubbing::WhilePaused;
        if (!m_media_element->paused()) {
            m_media_element->pause();
            m_scrubbing_timeline = Scrubbing::WhilePlaying;
        }

        set_current_time(*position);

        auto& realm = m_media_element->realm();
        auto& window = as<HTML::Window>(realm.global_object());

        auto mousemove_listener = add_event_listener(realm, window, UIEvents::EventNames::mousemove, [this](UIEvents::MouseEvent const& event) {
            VERIFY(m_media_element);
            VERIFY(m_dom->timeline_element);

            auto position = compute_timeline_position(event, *m_dom->timeline_element, m_media_element->duration());
            if (!position.has_value())
                return false;

            set_current_time(*position);
            return true;
        });

        add_event_listener(realm, window, UIEvents::EventNames::mouseup, ListenOnce::Yes, [this, mousemove_listener](UIEvents::MouseEvent const& event) {
            VERIFY(m_media_element);
            VERIFY(m_dom->timeline_element);

            auto was_playing = m_scrubbing_timeline == Scrubbing::WhilePlaying;
            m_scrubbing_timeline = Scrubbing::No;

            auto position = compute_timeline_position(event, *m_dom->timeline_element, m_media_element->duration());
            if (position.has_value())
                set_current_time(*position);

            if (was_playing) {
                if (m_media_element->ended()) {
                    auto loop = m_media_element->has_attribute(HTML::AttributeNames::loop);
                    if (loop)
                        m_media_element->play();
                } else {
                    m_media_element->play();
                }
            }

            update_play_pause_icon();

            auto& window_inner = static_cast<HTML::Window&>(relevant_global_object(*m_media_element));
            window_inner.remove_event_listener_without_options(UIEvents::EventNames::mousemove, mousemove_listener);
            return true;
        });

        return true;
    });

    // Speaker button
    add_event_listener(realm, *m_dom->mute_button, UIEvents::EventNames::click, [this] {
        VERIFY(m_media_element);
        m_media_element->set_muted(!m_media_element->muted());
        return true;
    });

    // Volume scrubbing
    static constexpr auto compute_volume = [](UIEvents::MouseEvent const& event, DOM::Element& volume_element) -> Optional<double> {
        auto rect = volume_element.get_bounding_client_rect();
        return clamp((event.client_x() - rect.left().to_double()) / rect.width().to_double(), 0.0, 1.0);
    };

    add_event_listener(realm, *m_dom->volume_area, UIEvents::EventNames::mousedown, [this](UIEvents::MouseEvent const& event) {
        VERIFY(m_media_element);
        VERIFY(m_dom->volume_element);

        auto volume = compute_volume(event, *m_dom->volume_element);
        if (!volume.has_value())
            return false;

        m_scrubbing_volume = true;

        set_volume(*volume);

        auto& realm = m_media_element->realm();
        auto& window = as<HTML::Window>(realm.global_object());

        auto mousemove_listener = add_event_listener(realm, window, UIEvents::EventNames::mousemove, [this](UIEvents::MouseEvent const& event) {
            VERIFY(m_media_element);
            VERIFY(m_dom->volume_element);

            auto volume = compute_volume(event, *m_dom->volume_element);
            if (!volume.has_value())
                return false;

            set_volume(*volume);
            return true;
        });

        add_event_listener(realm, window, UIEvents::EventNames::mouseup, ListenOnce::Yes, [this, mousemove_listener](UIEvents::MouseEvent const& event) {
            VERIFY(m_media_element);
            VERIFY(m_dom->volume_element);

            m_scrubbing_volume = false;

            auto volume = compute_volume(event, *m_dom->volume_element);
            if (volume.has_value())
                set_volume(*volume);

            auto& window_inner = static_cast<HTML::Window&>(relevant_global_object(*m_media_element));
            window_inner.remove_event_listener_without_options(UIEvents::EventNames::mousemove, mousemove_listener);
            return true;
        });

        return true;
    });

    // Fullscreen button
    if (m_dom->fullscreen_button) {
        add_event_listener(realm, *m_dom->fullscreen_button, UIEvents::EventNames::click, [this] {
            toggle_fullscreen();
            return true;
        });

        VERIFY(m_dom->video_overlay);
        add_event_listener(realm, *m_dom->video_overlay, UIEvents::EventNames::dblclick, [this] {
            toggle_fullscreen();
            return true;
        });
    }

    // Hover detection for video controls visibility
    if (is<HTMLVideoElement>(media_element)) {
        add_event_listener(realm, media_element, UIEvents::EventNames::mouseenter, [this] {
            show_controls();
            return true;
        });
        add_event_listener(realm, media_element, UIEvents::EventNames::mousemove, [this] {
            show_controls();
            return true;
        });
        add_event_listener(realm, media_element, UIEvents::EventNames::mouseleave, [this] {
            hide_controls();
            return true;
        });
        add_event_listener(realm, *m_dom->control_bar, UIEvents::EventNames::mouseenter, [this] {
            m_hovering_controls = true;
            show_controls();
            return true;
        });
        add_event_listener(realm, *m_dom->control_bar, UIEvents::EventNames::mouseleave, [this] {
            m_hovering_controls = false;
            show_controls();
            return true;
        });
    }

    // Keyboard handling
    add_event_listener(realm, media_element, UIEvents::EventNames::keydown, [this](UIEvents::KeyboardEvent const& event) {
        VERIFY(m_media_element);

        constexpr double arrow_time_step = 5.0;
        constexpr double arrow_volume_step = 0.1;

        auto key = event.key();

        if (key == " ") {
            toggle_playback();
        } else if (key == "Home") {
            set_current_time(0);
        } else if (key == "End") {
            set_current_time(m_media_element->duration());
        } else if (key == "ArrowLeft") {
            set_current_time(m_media_element->current_time() - arrow_time_step);
        } else if (key == "ArrowRight") {
            set_current_time(m_media_element->current_time() + arrow_time_step);
        } else if (key == "ArrowUp") {
            set_volume(m_media_element->volume() + arrow_volume_step);
        } else if (key == "ArrowDown") {
            set_volume(m_media_element->volume() - arrow_volume_step);
        } else if (key == "m" || key == "M") {
            toggle_mute();
        } else {
            return false;
        }

        return true;
    });

    // Use requestAnimationFrame to update the timeline, since timeupdate only fires every 250ms.
    auto request_animation_frame_callback_function = JS::NativeFunction::create(
        realm, [this](JS::VM&) {
            update_timeline();

            auto& realm = m_media_element->realm();
            auto& window = as<HTML::Window>(realm.global_object());
            window.request_animation_frame(*m_request_animation_frame_callback);

            return JS::js_undefined();
        },
        0, Utf16FlyString {}, &realm);
    m_request_animation_frame_callback = realm.heap().allocate<WebIDL::CallbackType>(request_animation_frame_callback_function, realm);
    window.request_animation_frame(*m_request_animation_frame_callback);
}

void MediaControls::toggle_playback()
{
    if (m_scrubbing_timeline != Scrubbing::No)
        return;
    m_media_element->toggle_playback();
    show_controls();
}

void MediaControls::set_current_time(double time)
{
    m_media_element->set_current_time(time);
    update_timeline();
    update_timestamp();
    show_controls();
}

void MediaControls::set_volume(double volume)
{
    volume = clamp(volume, 0.0, 1.0);
    MUST(m_media_element->set_volume(volume));
    m_media_element->set_muted(false);
    show_controls();
}

void MediaControls::toggle_mute()
{
    m_media_element->set_muted(!m_media_element->muted());
    show_controls();
}

void MediaControls::toggle_fullscreen()
{
    VERIFY(m_media_element);
    m_media_element->toggle_fullscreen();
}

void MediaControls::update_play_pause_icon()
{
    VERIFY(m_media_element);
    VERIFY(m_dom->play_pause_icon);

    auto paused = [&] {
        if (m_scrubbing_timeline != Scrubbing::No)
            return m_scrubbing_timeline == Scrubbing::WhilePaused;
        return m_media_element->paused();
    }();

    static String s_playing_class = "playing"_string;
    MUST(m_dom->play_pause_icon->class_list()->toggle(s_playing_class, !paused));
}

void MediaControls::update_timeline()
{
    VERIFY(m_media_element);
    VERIFY(m_dom->timeline_fill);

    auto duration = m_media_element->duration();
    double percentage = 0.0;
    if (!isnan(duration) && duration > 0.0)
        percentage = (m_media_element->current_time() / duration) * 100.0;

    if (m_last_timeline_percentage == percentage)
        return;

    MUST(m_dom->timeline_fill->style_for_bindings()->set_property(CSS::PropertyID::Width, MUST(String::formatted("{}%", percentage))));
    m_last_timeline_percentage = percentage;
}

void MediaControls::update_timestamp()
{
    VERIFY(m_media_element);
    VERIFY(m_dom->timestamp_element);

    auto current = human_readable_digital_time(round_to<i64>(m_media_element->current_time()));
    auto duration = m_media_element->duration();
    auto total = human_readable_digital_time(isnan(duration) ? 0 : round_to<i64>(duration));

    MUST(m_dom->timestamp_element->set_text_content(Utf16String::formatted("{} / {}", current, total)));
}

void MediaControls::update_volume_and_mute_indicator()
{
    VERIFY(m_media_element);
    VERIFY(m_dom->volume_fill);
    VERIFY(m_dom->mute_button);

    auto volume = m_media_element->volume();
    auto has_audio = m_media_element->audio_tracks()->length() > 0;
    auto muted = !has_audio || m_media_element->muted();

    if (muted) {
        MUST(m_dom->volume_fill->style_for_bindings()->set_property(CSS::PropertyID::Width, "0"sv));
    } else {
        auto percentage = volume * 100.0;
        MUST(m_dom->volume_fill->style_for_bindings()->set_property(CSS::PropertyID::Width, MUST(String::formatted("{}%", percentage))));
    }

    auto new_volume_icon_state = [&] {
        if (volume > 0.5)
            return MuteIconState::High;
        if (volume > 0)
            return MuteIconState::Low;
        return MuteIconState::Empty;
    }();

    static constexpr auto icon_class = [](MuteIconState state) {
        static Vector<String> s_no_volume_class = {};
        static Vector<String> s_low_volume_class = { "low"_string };
        static Vector<String> s_high_volume_class = { "high"_string };

        switch (state) {
        case MuteIconState::Empty:
            return s_no_volume_class;
        case MuteIconState::Low:
            return s_low_volume_class;
        case MuteIconState::High:
            return s_high_volume_class;
        }
        VERIFY_NOT_REACHED();
    };

    if (new_volume_icon_state != m_mute_icon_state) {
        MUST(m_dom->mute_button->class_list()->remove(icon_class(m_mute_icon_state)));
        MUST(m_dom->mute_button->class_list()->add(icon_class(new_volume_icon_state)));
        m_mute_icon_state = new_volume_icon_state;
    }

    static Vector<String> s_muted_class = { "muted"_string };
    if (muted != m_was_muted) {
        MUST(m_dom->mute_button->class_list()->toggle("muted"_string, muted));
        m_was_muted = muted;
    }

    static Vector<String> s_hidden_class = { "hidden"_string };
    if (has_audio != m_had_audio) {
        MUST(m_dom->volume_area->class_list()->toggle("hidden"_string, !has_audio));
        m_had_audio = has_audio;
    }
}

void MediaControls::update_fullscreen_icon()
{
    if (!m_dom->fullscreen_icon)
        return;

    static auto s_fullscreen_class = "fullscreen"_string;

    VERIFY(m_media_element);

    auto is_fullscreen_element = m_media_element->document().fullscreen_element() == m_media_element;
    MUST(m_dom->fullscreen_icon->class_list()->toggle(s_fullscreen_class, is_fullscreen_element));
}

void MediaControls::update_placeholder_visibility()
{
    VERIFY(m_media_element);

    if (!m_dom->placeholder_circle)
        return;

    auto display = should_show_placeholder() ? "flex"sv : "none"sv;
    MUST(m_dom->placeholder_circle->style_for_bindings()->set_property(CSS::PropertyID::Display, display));
}

bool MediaControls::should_show_placeholder() const
{
    VERIFY(m_media_element);
    VERIFY(m_dom->placeholder_circle);

    auto const& video_element = as<HTMLVideoElement>(*m_media_element);
    return video_element.current_representation() != HTMLVideoElement::Representation::VideoFrame;
}

static Vector<String> s_visible_class = { "visible"_string };

void MediaControls::show_controls()
{
    VERIFY(m_dom->control_bar);

    MUST(m_dom->control_bar->class_list()->add(s_visible_class));

    if (!m_hover_timer) {
        constexpr int hover_timeout_ms = 1000;
        m_hover_timer = Core::Timer::create_single_shot(hover_timeout_ms, [&] {
            hide_controls();
        });
        m_hover_timer->start();
    } else {
        m_hover_timer->restart();
    }
}

void MediaControls::hide_controls()
{
    VERIFY(m_dom->control_bar);

    if (m_scrubbing_timeline != Scrubbing::No || m_scrubbing_volume || m_hovering_controls)
        return;
    if (m_dom->placeholder_circle && should_show_placeholder())
        return;

    MUST(m_dom->control_bar->class_list()->remove(s_visible_class));
    m_hover_timer.clear();
}

}

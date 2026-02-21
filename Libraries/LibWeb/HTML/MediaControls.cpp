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
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/MediaControls.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::CSS {

extern String media_controls_stylesheet_source;

}

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
    if (auto media_element = m_media_element.ptr())
        media_element->set_shadow_root(nullptr);
}

static GC::Ref<DOM::Element> create_html_element(DOM::Document& document, FlyString const& tag, StringView class_name = {})
{
    auto element = MUST(DOM::create_element(document, tag, Namespace::HTML));
    if (!class_name.is_empty())
        element->set_attribute_value(HTML::AttributeNames::class_, String::from_utf8(class_name).release_value());
    return element;
}

static GC::Ref<DOM::Element> create_svg_element(DOM::Document& document, FlyString const& tag, StringView class_name = {})
{
    auto element = MUST(DOM::create_element(document, tag, Namespace::SVG));
    if (!class_name.is_empty())
        element->set_attribute_value(SVG::AttributeNames::class_, String::from_utf8(class_name).release_value());
    return element;
}

static String s_icon_view_box = "0 0 24 24"_string;

static GC::Ref<DOM::Element> create_play_icon(DOM::Document& document, StringView class_name)
{
    auto icon = create_svg_element(document, SVG::TagNames::svg, class_name);
    icon->set_attribute_value(SVG::AttributeNames::viewBox, s_icon_view_box);

    auto path = create_svg_element(document, SVG::TagNames::path, "play-path"sv);
    path->set_attribute_value(SVG::AttributeNames::d, "m6 5 13 7-13 7Z"_string);
    MUST(icon->append_child(path));

    return icon;
}

static GC::Ref<DOM::Element> create_mute_icon(DOM::Document& document, StringView class_name)
{
    auto icon = create_svg_element(document, SVG::TagNames::svg, class_name);
    icon->set_attribute_value(SVG::AttributeNames::viewBox, s_icon_view_box);

    // Muted clipping path
    auto defs = create_svg_element(document, SVG::TagNames::defs);
    MUST(icon->append_child(defs));

    auto muted_clip_path = create_svg_element(document, SVG::TagNames::clipPath);
    muted_clip_path->set_attribute_value(AttributeNames::id, "muted-clip"_string);
    MUST(defs->append_child(muted_clip_path));

    auto muted_clip_path_path = create_svg_element(document, SVG::TagNames::path);
    muted_clip_path_path->set_attribute_value(SVG::AttributeNames::d, "M3 0h21v21ZM0 0v24h24z"_string);
    MUST(muted_clip_path->append_child(muted_clip_path_path));

    // Muted cross-out line
    auto muted_line = create_svg_element(document, SVG::TagNames::path, "muted-line"_string);
    muted_line->set_attribute_value(SVG::AttributeNames::d, "m5 5 14 14-1.5 1.5-14-14z"_string);
    MUST(icon->append_child(muted_line));

    // High volume wave path
    auto volume_high = create_svg_element(document, SVG::TagNames::path, "volume-high"_string);
    volume_high->set_attribute_value(SVG::AttributeNames::d, "M14 4.08v2.04c2.23.55 4 2.9 4 5.88 0 2.97-1.77 5.33-4 5.88v2.04c3.45-.56 6-3.96 6-7.92s-2.55-7.36-6-7.92Z"_string);
    MUST(icon->append_child(volume_high));

    // Low volume wave path
    auto volume_low = create_svg_element(document, SVG::TagNames::path, "volume-low"_string);
    volume_low->set_attribute_value(SVG::AttributeNames::d, "M14 7.67v8.66c.35-.25.66-.55.92-.9A5.7 5.7 0 0 0 16 12c0-1.3-.39-2.5-1.08-3.43a4.24 4.24 0 0 0-.92-.9Z"_string);
    MUST(icon->append_child(volume_low));

    // Speaker path
    auto speaker = create_svg_element(document, SVG::TagNames::path, "speaker"_string);
    speaker->set_attribute_value(SVG::AttributeNames::d, "M4 9v6h4l4 5V4L8 9Z"_string);
    MUST(icon->append_child(speaker));

    return icon;
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

    // Scoped stylesheet
    auto style_element = create_html_element(document, HTML::TagNames::style);
    MUST(style_element->set_text_content(Utf16String::from_utf8(CSS::media_controls_stylesheet_source)));
    MUST(shadow_root->append_child(style_element));

    // Controls container
    auto controls_container = create_html_element(document, HTML::TagNames::div, is_video ? "container video"sv : "container audio"sv);
    MUST(shadow_root->append_child(controls_container));

    // Video overlay — covers the full video area to catch clicks for play/pause toggle.
    // Also contains the placeholder circle shown when no video data is available.
    if (is_video) {
        m_video_overlay = create_html_element(document, HTML::TagNames::div, "video-overlay"sv);
        MUST(controls_container->append_child(*m_video_overlay));

        m_placeholder_circle = create_html_element(document, HTML::TagNames::div, "placeholder-circle"sv);
        MUST(m_video_overlay->append_child(*m_placeholder_circle));

        auto placeholder_icon = create_play_icon(document, "placeholder-icon"sv);
        MUST(m_placeholder_circle->append_child(placeholder_icon));
    }

    // Control bar container
    m_control_bar = create_html_element(document, HTML::TagNames::div, "controls"sv);
    MUST(controls_container->append_child(*m_control_bar));

    // Timeline
    m_timeline_element = create_html_element(document, HTML::TagNames::div, "timeline"sv);
    MUST(m_control_bar->append_child(*m_timeline_element));
    m_timeline_fill = create_html_element(document, HTML::TagNames::div, "timeline-fill"sv);
    MUST(m_timeline_element->append_child(*m_timeline_fill));

    // Button bar
    auto button_bar = create_html_element(document, HTML::TagNames::div, "button-bar"sv);
    MUST(m_control_bar->append_child(button_bar));

    // Play/pause button
    m_play_button = create_html_element(document, HTML::TagNames::button, "control-button play-pause-button"sv);
    MUST(button_bar->append_child(*m_play_button));

    // Play/pause icon
    m_play_pause_icon = create_play_icon(document, "icon play-pause-icon"sv);
    MUST(m_play_button->append_child(*m_play_pause_icon));

    auto pause_path = create_svg_element(document, SVG::TagNames::path, "pause-path"sv);
    pause_path->set_attribute_value(SVG::AttributeNames::d, "M14 5h4v14h-4Zm-4 0H6v14h4z"_string);
    MUST(m_play_pause_icon->append_child(pause_path));

    // Timestamp
    m_timestamp_element = create_html_element(document, HTML::TagNames::span, "timestamp"sv);
    MUST(m_timestamp_element->set_text_content(Utf16String::from_utf8("0:00 / 0:00"sv)));
    MUST(button_bar->append_child(*m_timestamp_element));

    // Speaker button
    m_mute_button = create_html_element(document, HTML::TagNames::button, "control-button mute-button"sv);
    MUST(button_bar->append_child(*m_mute_button));

    auto mute_icon = create_mute_icon(document, "icon"sv);
    MUST(m_mute_button->append_child(mute_icon));

    // Volume slider
    m_volume_area = create_html_element(document, HTML::TagNames::div, "volume-area"sv);
    MUST(button_bar->append_child(*m_volume_area));
    m_volume_element = create_html_element(document, HTML::TagNames::div, "volume"sv);
    MUST(m_volume_area->append_child(*m_volume_element));
    m_volume_fill = create_html_element(document, HTML::TagNames::div, "volume-fill"sv);
    MUST(m_volume_element->append_child(*m_volume_fill));

    // Initialize state
    update_play_pause_icon();
    update_timestamp();
    update_volume_and_mute_indicator();
    update_placeholder_visibility();

    show_controls();
}

template<typename T, CallableAs<bool, T&> Handler>
GC::Ref<DOM::IDLEventListener> MediaControls::add_event_listener(JS::Realm& realm, DOM::EventTarget& target, FlyString const& event_name, ListenOnce listen_once, Handler handler)
{
    auto callback_function = JS::NativeFunction::create(
        realm, [handler = move(handler)](JS::VM& vm) {
            T* event = vm.argument(0).as_if<T>();
            if (event) {
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

    // Play/pause button
    add_event_listener(realm, *m_play_button, UIEvents::EventNames::click, [this] {
        toggle_playback();
        return true;
    });

    // Video overlay click — toggle playback when clicking outside the controls
    if (m_video_overlay) {
        add_event_listener(realm, *m_video_overlay, UIEvents::EventNames::click, [this] {
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

    add_event_listener(realm, *m_timeline_element, UIEvents::EventNames::mousedown, [this](UIEvents::MouseEvent const& event) {
        VERIFY(m_media_element);
        VERIFY(m_timeline_element);

        auto position = compute_timeline_position(event, *m_timeline_element, m_media_element->duration());
        if (!position.has_value())
            return false;

        m_scrubbing_timeline = Scrubbing::WhilePaused;
        if (!m_media_element->paused()) {
            m_media_element->pause();
            m_scrubbing_timeline = Scrubbing::WhilePlaying;
        }

        set_current_time(*position);

        auto& realm = m_media_element->realm();
        auto& window = static_cast<HTML::Window&>(relevant_global_object(*m_media_element));

        auto mousemove_listener = add_event_listener(realm, window, UIEvents::EventNames::mousemove, [this](UIEvents::MouseEvent const& event) {
            VERIFY(m_media_element);
            VERIFY(m_timeline_element);

            auto position = compute_timeline_position(event, *m_timeline_element, m_media_element->duration());
            if (!position.has_value())
                return false;

            set_current_time(*position);
            return true;
        });

        add_event_listener(realm, window, UIEvents::EventNames::mouseup, ListenOnce::Yes, [this, mousemove_listener](UIEvents::MouseEvent const& event) {
            VERIFY(m_media_element);
            VERIFY(m_timeline_element);

            auto was_playing = m_scrubbing_timeline == Scrubbing::WhilePlaying;
            m_scrubbing_timeline = Scrubbing::No;

            auto position = compute_timeline_position(event, *m_timeline_element, m_media_element->duration());
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
    add_event_listener(realm, *m_mute_button, UIEvents::EventNames::click, [this] {
        VERIFY(m_media_element);
        m_media_element->set_muted(!m_media_element->muted());
        return true;
    });

    // Volume scrubbing
    static constexpr auto compute_volume = [](UIEvents::MouseEvent const& event, DOM::Element& volume_element) -> Optional<double> {
        auto rect = volume_element.get_bounding_client_rect();
        return clamp((event.client_x() - rect.left().to_double()) / rect.width().to_double(), 0.0, 1.0);
    };

    add_event_listener(realm, *m_volume_area, UIEvents::EventNames::mousedown, [this](UIEvents::MouseEvent const& event) {
        VERIFY(m_media_element);
        VERIFY(m_volume_element);

        auto volume = compute_volume(event, *m_volume_element);
        if (!volume.has_value())
            return false;

        m_scrubbing_volume = true;

        set_volume(*volume);

        auto& realm = m_media_element->realm();
        auto& window = static_cast<HTML::Window&>(relevant_global_object(*m_media_element));

        auto mousemove_listener = add_event_listener(realm, window, UIEvents::EventNames::mousemove, [this](UIEvents::MouseEvent const& event) {
            VERIFY(m_media_element);
            VERIFY(m_volume_element);

            auto volume = compute_volume(event, *m_volume_element);
            if (!volume.has_value())
                return false;

            set_volume(*volume);
            return true;
        });

        add_event_listener(realm, window, UIEvents::EventNames::mouseup, ListenOnce::Yes, [this, mousemove_listener](UIEvents::MouseEvent const& event) {
            VERIFY(m_media_element);
            VERIFY(m_volume_element);

            m_scrubbing_volume = false;

            auto volume = compute_volume(event, *m_volume_element);
            if (volume.has_value())
                set_volume(*volume);

            auto& window_inner = static_cast<HTML::Window&>(relevant_global_object(*m_media_element));
            window_inner.remove_event_listener_without_options(UIEvents::EventNames::mousemove, mousemove_listener);
            return true;
        });

        return true;
    });

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
        add_event_listener(realm, *m_control_bar, UIEvents::EventNames::mouseenter, [this] {
            m_hovering_controls = true;
            show_controls();
            return true;
        });
        add_event_listener(realm, *m_control_bar, UIEvents::EventNames::mouseleave, [this] {
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

void MediaControls::update_play_pause_icon()
{
    VERIFY(m_media_element);
    VERIFY(m_play_pause_icon);

    auto paused = [&] {
        if (m_scrubbing_timeline != Scrubbing::No)
            return m_scrubbing_timeline == Scrubbing::WhilePaused;
        return m_media_element->paused();
    }();

    static String s_playing_class = "playing"_string;
    MUST(m_play_pause_icon->class_list()->toggle(s_playing_class, !paused));
}

void MediaControls::update_timeline()
{
    VERIFY(m_media_element);
    VERIFY(m_timeline_fill);

    auto duration = m_media_element->duration();
    double percentage = 0.0;
    if (!isnan(duration) && duration > 0.0)
        percentage = (m_media_element->current_time() / duration) * 100.0;

    MUST(m_timeline_fill->style_for_bindings()->set_property(CSS::PropertyID::Width, MUST(String::formatted("{}%", percentage))));
}

void MediaControls::update_timestamp()
{
    VERIFY(m_media_element);
    VERIFY(m_timestamp_element);

    auto current = human_readable_digital_time(round_to<i64>(m_media_element->current_time()));
    auto duration = m_media_element->duration();
    auto total = human_readable_digital_time(isnan(duration) ? 0 : round_to<i64>(duration));

    MUST(m_timestamp_element->set_text_content(Utf16String::formatted("{} / {}", current, total)));
}

void MediaControls::update_volume_and_mute_indicator()
{
    VERIFY(m_media_element);
    VERIFY(m_volume_fill);
    VERIFY(m_mute_button);

    auto volume = m_media_element->volume();
    auto has_audio = m_media_element->audio_tracks()->length() > 0;
    auto muted = !has_audio || m_media_element->muted();

    if (muted) {
        MUST(m_volume_fill->style_for_bindings()->set_property(CSS::PropertyID::Width, "0"sv));
    } else {
        auto percentage = volume * 100.0;
        MUST(m_volume_fill->style_for_bindings()->set_property(CSS::PropertyID::Width, MUST(String::formatted("{}%", percentage))));
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
        MUST(m_mute_button->class_list()->remove(icon_class(m_mute_icon_state)));
        MUST(m_mute_button->class_list()->add(icon_class(new_volume_icon_state)));
        m_mute_icon_state = new_volume_icon_state;
    }

    static Vector<String> s_muted_class = { "muted"_string };
    if (muted != m_was_muted) {
        MUST(m_mute_button->class_list()->toggle("muted"_string, muted));
        m_was_muted = muted;
    }

    static Vector<String> s_hidden_class = { "hidden"_string };
    if (has_audio != m_had_audio) {
        MUST(m_volume_area->class_list()->toggle("hidden"_string, !has_audio));
        m_had_audio = has_audio;
    }
}

void MediaControls::update_placeholder_visibility()
{
    VERIFY(m_media_element);

    if (!m_placeholder_circle)
        return;

    auto const& video_element = as<HTMLVideoElement>(*m_media_element);
    auto representation = video_element.current_representation();
    auto show_placeholder = representation != HTML::HTMLVideoElement::Representation::VideoFrame;
    MUST(m_placeholder_circle->style_for_bindings()->set_property(CSS::PropertyID::Display, show_placeholder ? "flex"_string : "none"_string));
}

static Vector<String> s_visible_class = { "visible"_string };

void MediaControls::show_controls()
{
    VERIFY(m_control_bar);

    MUST(m_control_bar->class_list()->add(s_visible_class));

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
    VERIFY(m_control_bar);

    if (m_scrubbing_timeline != Scrubbing::No || m_scrubbing_volume || m_hovering_controls)
        return;

    MUST(m_control_bar->class_list()->remove(s_visible_class));
    m_hover_timer.clear();
}

}

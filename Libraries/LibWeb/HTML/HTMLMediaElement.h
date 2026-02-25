/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <LibGC/RootVector.h>
#include <LibGfx/Rect.h>
#include <LibMedia/Forward.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/MediaControls.h>
#include <LibWeb/Painting/ExternalContentSource.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

enum class MediaSeekMode : u8 {
    Accurate,
    ApproximateForSpeed,
};

class SourceElementSelector;

class HTMLMediaElement : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLMediaElement, HTMLElement);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~HTMLMediaElement() override;

    virtual bool is_focusable() const override { return true; }

    virtual void adjust_computed_style(CSS::ComputedProperties& style) override;

    // NOTE: The function is wrapped in a GC::HeapFunction immediately.
    void queue_a_media_element_task(Function<void()>);

    void cancel_the_fetching_process();

    GC::Ptr<MediaError> error() const { return m_error; }
    void set_decoder_error(String error_message);

    String const& current_src() const { return m_current_src; }
    void select_resource();

    enum class NetworkState : u8 {
        Empty,
        Idle,
        Loading,
        NoSource,
    };
    NetworkState network_state() const { return m_network_state; }

    [[nodiscard]] GC::Ref<TimeRanges> buffered() const;
    [[nodiscard]] GC::Ref<TimeRanges> played() const;
    [[nodiscard]] GC::Ref<TimeRanges> seekable() const;

    static constexpr auto supported_video_subtypes = Array {
        "webm"sv,
        "mp4"sv,
        "mpeg"sv,
        "ogg"sv,
    };
    static constexpr auto supported_audio_subtypes = Array {
        "flac"sv,
        "mp3"sv,
        "mpeg"sv,
        "ogg"sv,
        "wav"sv,
        "webm"sv,
    };
    Bindings::CanPlayTypeResult can_play_type(StringView type) const;

    enum class ReadyState : u8 {
        HaveNothing,
        HaveMetadata,
        HaveCurrentData,
        HaveFutureData,
        HaveEnoughData,
    };
    ReadyState ready_state() const { return m_ready_state; }
    bool blocked() const;
    bool stalled() const;

    bool seeking() const { return m_seeking; }
    void set_seeking(bool);

    WebIDL::ExceptionOr<void> load();

    double current_time() const;
    void set_current_time(double);
    void fast_seek(double);

    double current_playback_position() const { return m_current_playback_position; }
    void set_current_playback_position(double);

    double duration() const;
    bool show_poster() const { return m_show_poster; }
    bool paused() const { return m_paused; }
    bool ended() const;
    bool potentially_playing() const;
    GC::Ref<WebIDL::Promise> play();
    void pause();
    void toggle_playback();

    double volume() const { return m_volume; }
    WebIDL::ExceptionOr<void> set_volume(double);

    double default_playback_rate() const { return m_default_playback_rate; }
    void set_default_playback_rate(double);

    double playback_rate() const { return m_playback_rate; }
    WebIDL::ExceptionOr<void> set_playback_rate(double);

    bool muted() const { return m_muted; }
    void set_muted(bool);

    void page_mute_state_changed(Badge<Page>);

    double effective_media_volume() const;

    GC::Ref<AudioTrackList> audio_tracks() const { return *m_audio_tracks; }
    GC::Ref<VideoTrackList> video_tracks() const { return *m_video_tracks; }
    GC::Ref<TextTrackList> text_tracks() const { return *m_text_tracks; }

    void set_audio_track_enabled(Badge<AudioTrack>, GC::Ptr<HTML::AudioTrack> audio_track, bool);

    void set_selected_video_track(Badge<VideoTrack>, GC::Ptr<HTML::VideoTrack> video_track);

    void update_video_frame_and_timeline();

    GC::Ref<TextTrack> add_text_track(Bindings::TextTrackKind kind, String const& label, String const& language);

    void create_controls();
    void destroy_controls();

    CORSSettingAttribute crossorigin() const { return m_crossorigin; }

    RefPtr<Media::DisplayingVideoSink> const& selected_video_track_sink() const { return m_selected_video_track_sink; }

    Painting::ExternalContentSource& ensure_external_content_source();

protected:
    HTMLMediaElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual void removed_from(DOM::Node* old_parent, DOM::Node& old_root) override;
    virtual void children_changed(ChildrenChangedMetadata const* metadata) override;

private:
    friend SourceElementSelector;

    virtual bool is_html_media_element() const final { return true; }

    struct EntireResource { };
    struct UntilEnd {
        u64 first;
    };
    using ByteRange = Variant<EntireResource, UntilEnd>;

    Task::Source media_element_event_task_source() const { return m_media_element_event_task_source.source; }

    WebIDL::ExceptionOr<void> load_element();

    void fetch_resource(URL::URL const&, ESCAPING Function<void(String)> failure_callback);
    struct FetchData;
    void fetch_resource(NonnullRefPtr<FetchData> const&, ByteRange const&);

    static Optional<String> verify_response_or_get_failure_reason(GC::Ref<Fetch::Infrastructure::Response>, ByteRange const&, NonnullRefPtr<FetchData> const&);

    void restart_fetch_at_offset(FetchData&, u64 offset);

    void set_up_playback_manager(NonnullRefPtr<FetchData> const&);
    enum class FetchingStatus : u8 {
        Ongoing,
        Complete,
    };
    void process_media_data(FetchingStatus);

    void handle_media_source_failure(Span<GC::Ref<WebIDL::Promise>> promises, String error_message);
    void forget_media_resource_specific_tracks();
    void set_ready_state(ReadyState);

    void on_audio_track_added(Media::Track const&);
    void on_video_track_added(Media::Track const&);
    void on_metadata_parsed();
    void on_playback_manager_state_change();
    void play_element();
    void pause_element();
    void seek_element(double playback_position, MediaSeekMode = MediaSeekMode::Accurate);
    void finish_seeking_element();
    void notify_about_playing();
    void set_show_poster(bool);
    void set_paused(bool);
    void set_duration(double);
    void set_ended(bool);

    void volume_or_muted_attribute_changed();
    void update_volume();

    bool is_eligible_for_autoplay() const;

    enum class PlaybackDirection : u8 {
        Forwards,
        Backwards,
    };
    PlaybackDirection direction_of_playback() const;

    bool has_ended_playback() const;
    void upon_has_ended_playback_possibly_changed();
    void reached_end_of_media_playback();

    void dispatch_time_update_event();

    enum class TimeMarchesOnReason : u8 {
        NormalPlayback,
        Other,
    };
    void time_marches_on(TimeMarchesOnReason = TimeMarchesOnReason::NormalPlayback);

    GC::RootVector<GC::Ref<WebIDL::Promise>> take_pending_play_promises();
    void resolve_pending_play_promises(ReadonlySpan<GC::Ref<WebIDL::Promise>> promises);
    void reject_pending_play_promises(ReadonlySpan<GC::Ref<WebIDL::Promise>> promises, GC::Ref<WebIDL::DOMException> error);

    // https://html.spec.whatwg.org/multipage/media.html#reject-pending-play-promises
    template<typename ErrorType>
    void reject_pending_play_promises(ReadonlySpan<GC::Ref<WebIDL::Promise>> promises, Utf16String message)
    {
        auto& realm = this->realm();

        auto error = ErrorType::create(realm, move(message));
        reject_pending_play_promises(promises, error);
    }

    // https://html.spec.whatwg.org/multipage/media.html#media-element-event-task-source
    UniqueTaskSource m_media_element_event_task_source;

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-error
    GC::Ptr<MediaError> m_error;

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-crossorigin
    CORSSettingAttribute m_crossorigin { CORSSettingAttribute::NoCORS };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-currentsrc
    String m_current_src;

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-networkstate
    NetworkState m_network_state { NetworkState::Empty };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-readystate
    ReadyState m_ready_state { ReadyState::HaveNothing };
    bool m_first_data_load_event_since_load_start { false };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-seeking
    bool m_seeking { false };

    // https://html.spec.whatwg.org/multipage/media.html#current-playback-position
    double m_current_playback_position { 0 };

    // https://html.spec.whatwg.org/multipage/media.html#official-playback-position
    double m_official_playback_position { 0 };

    // https://html.spec.whatwg.org/multipage/media.html#default-playback-start-position
    double m_default_playback_start_position { 0 };

    // https://html.spec.whatwg.org/multipage/media.html#show-poster-flag
    bool m_show_poster { true };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-duration
    double m_duration { NAN };

    // https://html.spec.whatwg.org/multipage/media.html#list-of-pending-play-promises
    Vector<GC::Ref<WebIDL::Promise>> m_pending_play_promises;

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-paused
    bool m_paused { true };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-ended
    bool m_ended { false };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-defaultplaybackrate
    double m_default_playback_rate { 1.0 };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-playbackrate
    double m_playback_rate { 1.0 };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-volume
    double m_volume { 1.0 };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-muted
    bool m_muted { false };

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-audiotracks
    GC::Ptr<AudioTrackList> m_audio_tracks;

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-videotracks
    GC::Ptr<VideoTrackList> m_video_tracks;

    // https://html.spec.whatwg.org/multipage/media.html#dom-media-texttracks
    GC::Ptr<TextTrackList> m_text_tracks;

    // https://html.spec.whatwg.org/multipage/media.html#can-autoplay-flag
    bool m_can_autoplay { true };

    // https://html.spec.whatwg.org/multipage/media.html#delaying-the-load-event-flag
    Optional<DOM::DocumentLoadEventDelayer> m_delaying_the_load_event;

    bool m_running_time_update_event_handler { false };
    Optional<MonotonicTime> m_last_time_update_event_time;

    GC::Ptr<DOM::DocumentObserver> m_document_observer;

    GC::Ptr<SourceElementSelector> m_source_element_selector;

    GC::Weak<Fetch::Infrastructure::FetchController> m_fetch_controller;
    u32 m_current_fetch_generation { 0 };

    OwnPtr<Media::PlaybackManager> m_playback_manager;
    GC::Ptr<VideoTrack> m_selected_video_track;
    RefPtr<Media::DisplayingVideoSink> m_selected_video_track_sink;

    bool m_loop_was_specified_when_reaching_end_of_media_resource { false };

    Optional<MediaControls> m_controls;

    bool m_has_enabled_preferred_audio_track { false };
    bool m_has_selected_preferred_video_track { false };

    RefPtr<Painting::ExternalContentSource> m_external_content_source;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLMediaElement>() const { return is_html_media_element(); }

}

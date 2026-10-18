/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/TextTrack.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

using TextTrackKind = Bindings::TextTrackKind;
using TextTrackMode = Bindings::TextTrackMode;

class TextTrack final : public DOM::EventTarget {
    WEB_WRAPPABLE(TextTrack, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(TextTrack);

public:
    // https://html.spec.whatwg.org/multipage/media.html#text-track-readiness-state
    enum class ReadinessState {
        NotLoaded,
        Loading,
        Loaded,
        FailedToLoad,
    };

    static GC::Ref<TextTrack> create();
    virtual ~TextTrack() override;

    TextTrackKind kind();
    void set_kind(TextTrackKind);

    Utf16String const& label();
    void set_label(Utf16View);

    Utf16String const& language();
    void set_language(Utf16View);

    Utf16String const& id();
    void set_id(Utf16View);

    TextTrackMode mode();
    void set_mode(TextTrackMode);

    GC::Ptr<TextTrackCueList> cues();
    GC::Ptr<TextTrackCueList> active_cues();

    void add_cue(GC::Ref<TextTrackCue>);
    WebIDL::ExceptionOr<void> remove_cue(GC::Ref<TextTrackCue>);
    void empty_text_track_list_of_cues();

    ReadonlySpan<GC::Ref<TextTrackCue>> text_track_list_of_cues() const { return m_text_track_list_of_cues; }
    void sort_text_track_list_of_cues();
    void unset_active_flag_of_cues();

    GC::Ptr<HTMLTrackElement> track_element() const { return m_track_element; }
    void set_track_element(GC::Ptr<HTMLTrackElement> track_element) { m_track_element = track_element; }

    GC::Ptr<HTMLMediaElement> media_element() const { return m_media_element; }
    void set_media_element(GC::Ptr<HTMLMediaElement> media_element) { m_media_element = media_element; }

    void set_oncuechange(WebIDL::CallbackType*);
    WebIDL::CallbackType* oncuechange();

    ReadinessState readiness_state() { return m_readiness_state; }
    void set_readiness_state(ReadinessState readiness_state);

    void register_observer(Badge<TextTrackObserver>, TextTrackObserver&);
    void unregister_observer(Badge<TextTrackObserver>, TextTrackObserver&);

private:
    TextTrack();

    virtual void visit_edges(Cell::Visitor&) override;

    TextTrackKind m_kind { TextTrackKind::Subtitles };
    Utf16String m_label {};
    Utf16String m_language {};

    Utf16String m_id {};

    TextTrackMode m_mode { TextTrackMode::Disabled };

    ReadinessState m_readiness_state { ReadinessState::NotLoaded };

    // https://html.spec.whatwg.org/multipage/media.html#text-track-list-of-cues
    Vector<GC::Ref<TextTrackCue>> m_text_track_list_of_cues;
    u64 m_next_cue_order_added { 0 };

    GC::Ptr<HTMLTrackElement> m_track_element;
    GC::Ptr<HTMLMediaElement> m_media_element;

    GC::Ptr<TextTrackCueList> m_cues;
    GC::Ptr<TextTrackCueList> m_active_cues;

    HashTable<GC::Ref<TextTrackObserver>> m_observers;
};

TextTrackKind text_track_kind_from_string(String);

}

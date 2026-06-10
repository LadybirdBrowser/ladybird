/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
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

    String label();
    void set_label(String);

    String language();
    void set_language(String);

    String id();
    void set_id(String);

    TextTrackMode mode();
    void set_mode(TextTrackMode);

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
    String m_label {};
    String m_language {};

    String m_id {};

    TextTrackMode m_mode { TextTrackMode::Disabled };

    ReadinessState m_readiness_state { ReadinessState::NotLoaded };

    HashTable<GC::Ref<TextTrackObserver>> m_observers;
};

TextTrackKind text_track_kind_from_string(String);

}

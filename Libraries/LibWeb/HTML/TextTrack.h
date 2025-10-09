/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/TextTrackPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class TextTrack final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(TextTrack, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(TextTrack);

public:
    // https://html.spec.whatwg.org/multipage/media.html#text-track-readiness-state
    enum class ReadinessState {
        NotLoaded,
        Loading,
        Loaded,
        FailedToLoad,
    };

    static GC::Ref<TextTrack> create(JS::Realm&);
    virtual ~TextTrack() override;

    Bindings::TextTrackKind kind();
    void set_kind(Bindings::TextTrackKind);

    String label();
    void set_label(String);

    String language();
    void set_language(String);

    String id();
    void set_id(String);

    Bindings::TextTrackMode mode();
    void set_mode(Bindings::TextTrackMode);

    void set_oncuechange(WebIDL::CallbackType*);
    WebIDL::CallbackType* oncuechange();

    ReadinessState readiness_state() { return m_readiness_state; }
    void set_readiness_state(ReadinessState readiness_state);

    void register_observer(Badge<TextTrackObserver>, TextTrackObserver&);
    void unregister_observer(Badge<TextTrackObserver>, TextTrackObserver&);

private:
    TextTrack(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Bindings::TextTrackKind m_kind { Bindings::TextTrackKind::Subtitles };
    String m_label {};
    String m_language {};

    String m_id {};

    Bindings::TextTrackMode m_mode { Bindings::TextTrackMode::Disabled };

    ReadinessState m_readiness_state { ReadinessState::NotLoaded };

    HashTable<GC::Ref<TextTrackObserver>> m_observers;
};

Bindings::TextTrackKind text_track_kind_from_string(String const&);

}

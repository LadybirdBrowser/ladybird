/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/MarkedVector.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/TextTrack.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/media.html#texttrackcue
class TextTrackCue : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(TextTrackCue, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(TextTrackCue);

public:
    virtual ~TextTrackCue() override;

    JS::GCPtr<TextTrack> track() { return m_track; }

    String const& id() const { return m_identifier; }
    void set_id(String const& id) { m_identifier = id; }

    double start_time() const { return m_start_time; }
    void set_start_time(double start_time);

    double end_time() const { return m_end_time; }
    WebIDL::ExceptionOr<void> set_end_time(double end_time);

    bool pause_on_exit() const { return m_pause_on_exit; }
    void set_pause_on_exit(bool pause_on_exit) { m_pause_on_exit = pause_on_exit; }

    WebIDL::CallbackType* onenter();
    void set_onenter(WebIDL::CallbackType*);

    WebIDL::CallbackType* onexit();
    void set_onexit(WebIDL::CallbackType*);

protected:
    TextTrackCue(JS::Realm&, JS::GCPtr<TextTrack>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    JS::GCPtr<TextTrack> m_track;

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-identifier
    String m_identifier;

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-start-time
    double m_start_time;

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-end-time
    double m_end_time;

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-pause-on-exit-flag
    bool m_pause_on_exit;
};

}

/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/RootVector.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/TextTrack.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/media.html#texttrackcue
class TextTrackCue : public DOM::EventTarget {
    WEB_WRAPPABLE(TextTrackCue, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(TextTrackCue);

public:
    virtual ~TextTrackCue() override;

    GC::Ptr<TextTrack> track() const { return m_track; }
    void set_track(GC::Ptr<TextTrack> track) { m_track = track; }
    static constexpr size_t track_offset() { return offsetof(TextTrackCue, m_track); }

    Utf16String const& id() const { return m_identifier; }
    void set_id(Utf16View id) { m_identifier = Utf16String::from_utf16(id); }

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

    bool is_active() const { return m_active; }
    void set_active(bool active) { m_active = active; }

    u64 order_added() const { return m_order_added; }
    void set_order_added(u64 order_added) { m_order_added = order_added; }

protected:
    TextTrackCue(GC::Ptr<TextTrack>);

    void run_time_marches_on_if_needed();
    virtual void visit_edges(Visitor&) override;

    GC::Ptr<TextTrack> m_track;

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-identifier
    Utf16String m_identifier;

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-start-time
    double m_start_time;

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-end-time
    double m_end_time;

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-pause-on-exit-flag
    bool m_pause_on_exit;

    // https://html.spec.whatwg.org/multipage/media.html#text-track-cue-active-flag
    bool m_active { false };

    u64 m_order_added { 0 };
};

}

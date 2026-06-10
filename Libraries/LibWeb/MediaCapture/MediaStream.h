/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>

namespace Web::MediaCapture {

// Spec: https://w3c.github.io/mediacapture-main/#mediastream
class MediaStream final : public DOM::EventTarget {
    WEB_WRAPPABLE(MediaStream, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaStream);

public:
    static GC::Ref<MediaStream> create();
    static GC::Ref<MediaStream> create(ReadonlySpan<GC::Ref<MediaStreamTrack>> const&);
    static GC::Ref<MediaStream> create(GC::RootVector<GC::Ref<MediaStreamTrack>> const&);

    virtual ~MediaStream() override = default;

    String id() const { return m_id; }

    Vector<GC::Ref<MediaStreamTrack>> get_audio_tracks() const;
    Vector<GC::Ref<MediaStreamTrack>> get_video_tracks() const;
    Vector<GC::Ref<MediaStreamTrack>> get_tracks() const;
    GC::Ptr<MediaStreamTrack> get_track_by_id(String const& track_id) const;

    void add_track(GC::Ref<MediaStreamTrack> track);
    void append_track(GC::Ref<MediaStreamTrack>);
    void remove_track(GC::Ref<MediaStreamTrack> track);

    GC::Ref<MediaStream> clone() const;
    bool active() const;

    void set_onaddtrack(WebIDL::CallbackType* event_handler);
    WebIDL::CallbackType* onaddtrack();
    void set_onremovetrack(WebIDL::CallbackType* event_handler);
    WebIDL::CallbackType* onremovetrack();

private:
    explicit MediaStream();
    virtual void visit_edges(Cell::Visitor&) override;

    void add_track(JS::Object& global_object, GC::Ref<MediaStreamTrack> track);
    void remove_track(JS::Object& global_object, GC::Ref<MediaStreamTrack> track);

    String m_id;
    Vector<GC::Ref<MediaStreamTrack>> m_tracks;
};

}

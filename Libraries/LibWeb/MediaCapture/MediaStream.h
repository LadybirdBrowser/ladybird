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
    WEB_PLATFORM_OBJECT(MediaStream, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaStream);

public:
    static GC::Ref<MediaStream> create(JS::Realm&);
    static GC::Ref<MediaStream> construct_impl(JS::Realm&, GC::RootVector<GC::Root<MediaStreamTrack>> const& tracks);

    virtual ~MediaStream() override = default;

    String id() const { return m_id; }

    Vector<GC::Ref<MediaStreamTrack>> get_audio_tracks() const;
    Vector<GC::Ref<MediaStreamTrack>> get_video_tracks() const;
    Vector<GC::Ref<MediaStreamTrack>> get_tracks() const;
    GC::Ptr<MediaStreamTrack> get_track_by_id(String const& track_id) const;

    void add_track(GC::Ref<MediaStreamTrack> track);
    void remove_track(GC::Ref<MediaStreamTrack> track);

    GC::Ref<MediaStream> clone() const;
    bool active() const;

    void set_onaddtrack(WebIDL::CallbackType* event_handler);
    WebIDL::CallbackType* onaddtrack();
    void set_onremovetrack(WebIDL::CallbackType* event_handler);
    WebIDL::CallbackType* onremovetrack();

private:
    explicit MediaStream(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    String m_id;
    Vector<GC::Ref<MediaStreamTrack>> m_tracks;
};

}

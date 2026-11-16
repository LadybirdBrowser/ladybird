/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGC/Weak.h>
#include <LibMedia/I420Picture.h>
#include <LibWeb/Forward.h>

namespace Web::MediaCapture {

// Something that video comes from as pictures, like a camera or a canvas, and the tracks it is the source of. A track
// and its clones share a source, which does its work for as long as any of them is live.
// https://w3c.github.io/mediacapture-main/#track-source
class VideoPictureSource : public RefCounted<VideoPictureSource> {
public:
    virtual ~VideoPictureSource();

    void add_track(MediaStreamTrack&);
    void remove_track(MediaStreamTrack&);
    bool has_tracks() const { return !m_tracks.is_empty(); }

    void deliver(NonnullRefPtr<Media::I420Picture> const&);
    void end_tracks();
    void set_tracks_muted(bool);
    // End tracks for a lost source and notify their pages in a task.
    void end_tracks_for_lost_source();

protected:
    virtual void did_gain_first_track() { }
    virtual void did_lose_last_track() { }

private:
    Vector<GC::Weak<MediaStreamTrack>> m_tracks;
};

}

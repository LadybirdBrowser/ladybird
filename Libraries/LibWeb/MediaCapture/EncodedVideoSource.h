/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>

namespace Web::MediaCapture {

struct EncodedVideoFrame;

// Something that video comes from already encoded, like a WebRTC receiver, and the tracks it is the source of. A track
// and its clones share a source, which gives them all its frames and hears all their requests for key frames.
// https://w3c.github.io/mediacapture-main/#track-source
class EncodedVideoSource final
    : public RefCounted<EncodedVideoSource>
    , public Weakable<EncodedVideoSource> {
public:
    static NonnullRefPtr<EncodedVideoSource> create(Function<void()> request_key_frame);

    void add_track(MediaStreamTrack&);
    void remove_track(MediaStreamTrack&);
    bool has_tracks() const;

    void deliver(EncodedVideoFrame const&);

    void request_key_frame();
    // For a source whose frames come to it by another way than before, which it asks for key frames by.
    void set_key_frame_request_handler(Function<void()>);

private:
    explicit EncodedVideoSource(Function<void()> request_key_frame);

    Vector<GC::Weak<MediaStreamTrack>> m_tracks;
    Function<void()> m_request_key_frame;
};

}

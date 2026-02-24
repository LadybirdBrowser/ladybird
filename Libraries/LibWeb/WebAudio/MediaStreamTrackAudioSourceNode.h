/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#MediaStreamTrackAudioSourceOptions
struct MediaStreamTrackAudioSourceOptions {
    GC::Ptr<MediaCapture::MediaStreamTrack> media_stream_track;
};

// https://webaudio.github.io/web-audio-api/#MediaStreamTrackAudioSourceNode
class MediaStreamTrackAudioSourceNode final : public AudioNode {
    WEB_PLATFORM_OBJECT(MediaStreamTrackAudioSourceNode, AudioNode);
    GC_DECLARE_ALLOCATOR(MediaStreamTrackAudioSourceNode);

public:
    virtual ~MediaStreamTrackAudioSourceNode() override = default;

    static WebIDL::ExceptionOr<GC::Ref<MediaStreamTrackAudioSourceNode>> create(JS::Realm&, GC::Ref<AudioContext>, MediaStreamTrackAudioSourceOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<MediaStreamTrackAudioSourceNode>> construct_impl(JS::Realm&, GC::Ref<AudioContext>, MediaStreamTrackAudioSourceOptions const&);

    virtual WebIDL::UnsignedLong number_of_inputs() override { return 0; }
    virtual WebIDL::UnsignedLong number_of_outputs() override { return 1; }

    GC::Ref<MediaCapture::MediaStreamTrack> track() const { return m_track; }
    u64 provider_id() const { return m_provider_id; }

private:
    MediaStreamTrackAudioSourceNode(JS::Realm&, GC::Ref<AudioContext>, GC::Ref<MediaCapture::MediaStreamTrack>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<MediaCapture::MediaStreamTrack> m_track;
    u64 m_provider_id { 0 };
};

}
